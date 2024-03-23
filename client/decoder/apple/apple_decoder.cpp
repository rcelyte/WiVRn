#include "apple_decoder.h"
#include "application.h"
#include "scenes/stream.h"
#include <spdlog/spdlog.h>
#include <numeric>
using UnitType = wivrn::apple::decoder::DecodeState::UnitType;

wivrn::apple::decoder::decoder(vk::raii::Device &device, vk::raii::PhysicalDevice &physical_device,
		const xrt::drivers::wivrn::to_headset::video_stream_description::item &description, const float fps, const uint8_t stream_index,
		const std::weak_ptr<scenes::stream> scene, shard_accumulator *const accumulator) :
		description(description), device(device), weak_scene(scene), accumulator(accumulator),
		frameSampler(device, {
			.magFilter = vk::Filter::eLinear,
			.minFilter = vk::Filter::eLinear,
			.mipmapMode = vk::SamplerMipmapMode::eNearest,
			.addressModeU = vk::SamplerAddressMode::eClampToEdge,
			.addressModeV = vk::SamplerAddressMode::eClampToEdge,
			.addressModeW = vk::SamplerAddressMode::eClampToEdge,
			.borderColor = vk::BorderColor::eFloatOpaqueWhite,
			.unnormalizedCoordinates = VK_FALSE,
		}), state{.hevc = (description.codec == h265)} {
	spdlog::info("wivrn::apple::decoder()");
}

wivrn::apple::decoder::~decoder() {
	free(this->frameData.data());
	this->frameData = {};
	if(this->state.session != nullptr) {
		VTDecompressionSessionWaitForAsynchronousFrames(this->state.session);
		VTDecompressionSessionInvalidate(this->state.session);
		CFRelease(this->state.session);
	}
	if(this->state.formatDesc != nullptr)
		CFRelease(this->state.formatDesc);
	for(std::vector<uint8_t> &set : this->state.paramSets)
		set.clear();
	this->state = {};
}

void wivrn::apple::decoder::push_data(const std::span<std::span<const uint8_t>> shards, const uint64_t frame_index, const bool partial) {
	if(frame_index != this->frameIndex) {
		free(this->frameData.data());
		this->frameData = {};
		this->frameIndex = frame_index;
	}
	if(shards.size() == 0)
		return;
	const size_t fragmentLength = std::accumulate(shards.begin(), shards.end(), (size_t)0,
		[](const size_t total, const std::span<const uint8_t> &sub) {return total + sub.size();});
	this->frameData = {(uint8_t*)realloc(this->frameData.data(), this->frameData.size() + fragmentLength), this->frameData.size() + fragmentLength};
	if(this->frameData.data() == nullptr)
		throw std::runtime_error("realloc() failed");
	for(uint8_t *head = &*this->frameData.end() - fragmentLength; const std::span<const uint8_t> &sub : shards) {
		memcpy(head, sub.data(), sub.size());
		head += sub.size();
	}
}

static void onDecodeFrame(void *const userptr, void*, const OSStatus status, const VTDecodeInfoFlags infoFlags,
		const CVImageBufferRef imageBuffer, const CMTime presentationTimeStamp, const CMTime presentationDuration) {
	wivrn::apple::decoder *const decoder = (wivrn::apple::decoder*)userptr;
	if(status != noErr || imageBuffer == nullptr) {
		spdlog::error("frame decode failed with error {}", status);
		if(status == kVTVideoDecoderBadDataErr)
			abort();
		return;
	}
	if(CVPixelBufferIsPlanar(imageBuffer)) {
		spdlog::error("decoded image buffer should not be planar");
		return;
	}
	vk::StructureChain imageInfo = {
		vk::ImageCreateInfo{
			.flags = {},
			.imageType = vk::ImageType::e2D,
			.format = vk::Format::eR8G8B8A8Unorm,
			.extent = {
				.width = (uint32_t)CVPixelBufferGetWidth(imageBuffer),
				.height = (uint32_t)CVPixelBufferGetHeight(imageBuffer),
				.depth = 1,
			},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = vk::SampleCountFlagBits::e1,
			.tiling = vk::ImageTiling::eOptimal,
			.usage = vk::ImageUsageFlagBits::eSampled,
			.sharingMode = vk::SharingMode::eExclusive,
			.initialLayout = vk::ImageLayout::eUndefined,
		},
		vk::ImportMetalIOSurfaceInfoEXT{
			.ioSurface = CVPixelBufferGetIOSurface(imageBuffer),
		},
	};
	vk::raii::Image image(decoder->device, imageInfo.get<vk::ImageCreateInfo>());
	vk::raii::ImageView imageView(decoder->device, {
		.image = *image,
		.viewType = vk::ImageViewType::e2D,
		.format = imageInfo.get<vk::ImageCreateInfo>().format,
		.components = {vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eIdentity},
		.subresourceRange = {
			.aspectMask = vk::ImageAspectFlagBits::eColor,
			.levelCount = 1,
			.layerCount = 1,
		}
	});
	const std::shared_ptr<wivrn::apple::decoder::blit_handle> handle = std::make_shared<wivrn::apple::decoder::blit_handle>(
		decoder->pendingInfo, imageBuffer, std::move(image), std::move(imageView));
	if(auto scene = decoder->weak_scene.lock())
		scene->push_blit_handle(decoder->accumulator, std::move(handle));
}

template<bool avcc>
static inline UnitType ReadNalClass(uint8_t **data, const uint8_t *const data_end, const bool hevc) {
	if((size_t)(data_end - *data) < 5)
		return UnitType::None;
	*data += 3 + (avcc || (*data)[2] == 0);
	const uint8_t type = hevc ? ((**data >> 1) & 0x3f) - 32 : (**data & 0x1f) - 6;
	return (hevc || type != UnitType::VPS) ? (UnitType)std::min<uint8_t>(type, (uint8_t)UnitType::None) : UnitType::None;
}

void wivrn::apple::decoder::frame_completed(const xrt::drivers::wivrn::from_headset::feedback &feedback,
		const xrt::drivers::wivrn::to_headset::video_stream_data_shard::timing_info_t &timing_info,
		const xrt::drivers::wivrn::to_headset::video_stream_data_shard::view_info_t &view_info) {
	std::span<uint8_t> heldData = this->frameData;
	this->frameData = {};
	if(heldData.size() < 5) {
		free(heldData.data());
		return;
	}
	if(*(const uint32_t*)heldData.data() != 0x1000000) {
		spdlog::error("Need four byte prefix for in-place Annex-B fixup");
		abort(); // return;
	}
	for(uint8_t *head = heldData.data(), *next; head < &*heldData.end(); head = next) {
		constexpr std::array<uint8_t, 3> header = {{0, 0, 1}};
		next = std::search(&head[3], &*heldData.end(), header.begin(), header.end());
		if(next < &*heldData.end() && *(--next) != 0) {
			spdlog::error("Need four byte prefix for in-place Annex-B fixup");
			free(heldData.data());
			abort(); // return;
		}
		const uint32_t prefix = __builtin_bswap32((uint32_t)size_t(next - &head[sizeof(prefix)]));
		memcpy(head, &prefix, sizeof(prefix));
		const UnitType type = ReadNalClass<true>(&head, next, this->state.hevc);
		if(type == UnitType::None)
			continue;
		this->state.paramSets[type].clear();
		if(next == &head[1]) {
			spdlog::error("Skipping empty NAL unit");
			continue;
		}
		this->state.paramSets[type].resize((size_t)(next - head));
		memcpy(this->state.paramSets[type].data(), head, (size_t)(next - head));
		if(const UnitType nextUnit = UnitType(type + 1u); nextUnit < UnitType::None)
			this->state.paramSets[nextUnit].clear();
		if(this->state.paramSets[UnitType::SPS].empty() || this->state.paramSets[UnitType::PPS].empty() ||
				(this->state.hevc && this->state.paramSets[UnitType::VPS].empty()))
			continue;
		if(this->state.formatDesc != nullptr)
			CFRelease(this->state.formatDesc);
		{
			size_t paramLengths[UnitType::None];
			const uint8_t *paramSets[UnitType::None];
			for(uint32_t i = 0; i < UnitType::None; ++i) {
				paramLengths[i] = this->state.paramSets[i].size();
				paramSets[i] = this->state.paramSets[i].data();
			}
			const OSStatus result = this->state.hevc ?
				CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, paramSets, paramLengths, sizeof(uint32_t), nullptr, &this->state.formatDesc) :
				CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, &paramSets[UnitType::SPS], &paramLengths[UnitType::SPS], sizeof(uint32_t),
					&this->state.formatDesc);
			if(result != noErr) {
				spdlog::error("CMVideoFormatDescriptionCreateFrom{}ParameterSets() failed with error {}", this->state.hevc ? "HEVC" : "H264", result);
				this->state.formatDesc = nullptr;
			}
		}
		for(std::vector<uint8_t> &set : this->state.paramSets)
			set.clear();
		if(this->state.session == nullptr || (this->state.formatDesc != nullptr && VTDecompressionSessionCanAcceptFormatDescription(this->state.session, this->state.formatDesc)))
			continue;
		VTDecompressionSessionWaitForAsynchronousFrames(this->state.session);
		VTDecompressionSessionInvalidate(this->state.session);
		CFRelease(this->state.session);
		this->state.session = nullptr;
	}
	if(heldData.size() == 0 || this->state.formatDesc == nullptr)
		return;
	if(this->state.session != nullptr) {
		VTDecompressionSessionWaitForAsynchronousFrames(this->state.session); // ensures `this->pendingInfo` isn't in use
	} else {
		const uint32_t size[2] = {this->description.video_width, this->description.video_height};
		// const uint32_t format = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange; // VK_KHR_sampler_ycbcr_conversion seems broken in MoltenVK
		const uint32_t format = kCVPixelFormatType_32BGRA; // Available Pixel Formats - https://developer.apple.com/library/archive/qa/qa1501/_index.html
		const CFMutableDictionaryRef options =
			CFDictionaryCreateMutable(kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		// TODO: see if we can call `CFRelease()` earlier for the `CFNumberRef`s and if so switch to `std::unique_ptr(CFNumberCreate(...), CFRelease).get()`
		const CFNumberRef widthNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &size[0]);
		CFDictionarySetValue(options, kCVPixelBufferWidthKey, widthNum);
		const CFNumberRef heightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &size[1]);
		CFDictionarySetValue(options, kCVPixelBufferHeightKey, heightNum);
		const CFNumberRef formatNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &format);
		CFDictionarySetValue(options, kCVPixelBufferPixelFormatTypeKey, formatNum);
		CFDictionarySetValue(options, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
		const VTDecompressionOutputCallbackRecord callback = {onDecodeFrame, this};
		VTDecompressionSessionRef newSession;
		const OSStatus result = VTDecompressionSessionCreate(nullptr, this->state.formatDesc, nullptr, options, &callback, &newSession);
		CFRelease(widthNum);
		CFRelease(heightNum);
		CFRelease(formatNum);
		CFRelease(options);
		if(result != noErr) {
			spdlog::error("VTDecompressionSessionCreate() failed with error {}", result);
			return;
		}
		if(!VTDecompressionSessionCanAcceptFormatDescription(newSession, this->state.formatDesc)) {
			VTDecompressionSessionInvalidate(newSession);
			CFRelease(newSession);
			spdlog::error("VTDecompressionSessionCanAcceptFormatDescription() failed");
			return;
		}
		this->state.session = newSession;
	}
	CMBlockBufferRef inputFrame = nullptr;
	OSStatus result = CMBlockBufferCreateWithMemoryBlock(nullptr, heldData.data(), heldData.size(),
		kCFAllocatorDefault, nullptr, 0, heldData.size(), 0, &inputFrame); // TODO: use std::vector instead of manual memory management
	if(result != noErr) {
		spdlog::error("CMBlockBufferCreateWithMemoryBlock() failed with error {}", result);
		free(heldData.data());
		return;
	}
	CMSampleBufferRef sampleBuffer = nullptr;
	result = CMSampleBufferCreateReady(kCFAllocatorDefault, inputFrame, this->state.formatDesc, 1, 0, nullptr, 0, nullptr, &sampleBuffer);
	CFRelease(inputFrame);
	if(result != noErr) {
		spdlog::error("CMSampleBufferCreateReady() failed with error {}", result);
		CFRelease(sampleBuffer);
		return;
	}
	this->pendingInfo = {feedback, timing_info, view_info};
	result = VTDecompressionSessionDecodeFrame(this->state.session, sampleBuffer, kVTDecodeFrame_EnableAsynchronousDecompression, nullptr, nullptr);
	CFRelease(sampleBuffer);
	if(result != noErr)
		spdlog::error("VTDecompressionSessionDecodeFrame() failed with error {}", result);
}

wivrn::apple::decoder::blit_handle::blit_handle(const struct frame_info &info, const CVImageBufferRef frameRef, vk::raii::Image &&image,
		vk::raii::ImageView &&view) : frame_info(info), frameRef(CFRetain(frameRef)), imageHandle(std::move(image)),
		viewHandle(std::move(view)), image_view(this->viewHandle), image(*this->imageHandle), current_layout(&this->layout) {}

wivrn::apple::decoder::blit_handle::~blit_handle() {
	CFRelease(this->frameRef);
}

std::vector<xrt::drivers::wivrn::video_codec> wivrn::apple::decoder::supported_codecs()
{
	std::vector<xrt::drivers::wivrn::video_codec> result = {};
	if(VTIsHardwareDecodeSupported(kCMVideoCodecType_H264))
		result.push_back(xrt::drivers::wivrn::video_codec::h264);
	if(VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC))
		result.push_back(xrt::drivers::wivrn::video_codec::h265);
	return result;
}
