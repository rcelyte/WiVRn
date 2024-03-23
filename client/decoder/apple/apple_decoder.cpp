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
	if(!(this->state.hevc && VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC)) && description.codec != h264)
		throw std::runtime_error{"Unsupported video codec"};

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
	for(uint8_t *&set : this->state.paramSets) {
		free(set);
		set = nullptr;
	}
	this->state = {};
}

template<bool avcc>
static inline UnitType ReadNalClass(uint8_t **data, const uint8_t *const data_end, const bool hevc) {
	if((size_t)(data_end - *data) < 5)
		return UnitType::None;
	const bool fourByte = avcc || ((*data)[2] == 0);
	uint8_t *const type = &(*data)[3 + fourByte];
	*data = type;
	if(hevc) {
		switch((*type >> 1) & 0x3f) {
			case 32: return UnitType::VPS;
			case 33: return UnitType::SPS;
			case 34: return UnitType::PPS;
			default:;
		}
	} else {
		switch(*type & 0x1f) {
			case 7: return UnitType::SPS;
			case 8: return UnitType::PPS;
			default:;
		}
	}
	return UnitType::None;
}

void wivrn::apple::decoder::push_data(const std::span<std::span<const uint8_t>> shards, const uint64_t frame_index, const bool partial) {
	// spdlog::debug("push_data({})", frame_index);
	free(this->frameData.data());
	this->frameData = {};
	if(shards.size() == 0)
		return;
	const std::array<uint8_t, 3> header = {{0, 0, 1}};
	const bool needPad = (shards[0].size() >= header.size() && memcmp(shards[0].data(), header.data(), header.size()) == 0);
	assert(!needPad);
	const size_t frameData_len = std::accumulate(shards.begin(), shards.end(), (size_t)needPad,
		[](const size_t total, const std::span<const uint8_t> &sub) {return total + sub.size();});
	if(frameData_len < 5)
		return;
	this->frameData = {(uint8_t*)malloc(frameData_len), frameData_len};
	if(this->frameData.data() == nullptr) {
		this->frameData = {};
		return;
	}
	for(uint8_t *head = &this->frameData[needPad]; const std::span<const uint8_t> &sub : shards) {
		memcpy(head, sub.data(), sub.size());
		head += sub.size();
	}
	for(uint8_t *head = &this->frameData[needPad], *next; head < &*this->frameData.end(); head = next) {
		next = std::search(&head[3], &*this->frameData.end(), header.begin(), header.end());
		if(next < &*this->frameData.end() && *(--next) != 0) {
			spdlog::error("Need four byte prefix for in-place Annex-B fixup");
			free(this->frameData.data());
			this->frameData = {};
			return;
		}
		const uint32_t prefix = __builtin_bswap32((uint32_t)size_t(next - &head[sizeof(prefix)]));
		memcpy(head, &prefix, sizeof(prefix));
		const UnitType type = ReadNalClass<true>(&head, next, this->state.hevc);
		if(type == UnitType::None)
			continue;
		free(this->state.paramSets[type]);
		this->state.paramSets[type] = (uint8_t*)malloc((size_t)(next - head));
		this->state.paramLengths[type] = (size_t)(next - head);
		assert(this->state.paramSets[type] != nullptr);
		memcpy(this->state.paramSets[type], head, (size_t)(next - head));
		if(const UnitType next = UnitType(type + 1u); next < UnitType::None) {
			free(this->state.paramSets[next]);
			this->state.paramSets[next] = nullptr;
		}
		if(this->state.paramSets[UnitType::SPS] == nullptr || this->state.paramSets[UnitType::PPS] == nullptr ||
				(this->state.hevc && this->state.paramSets[UnitType::VPS] == nullptr))
			continue;
		if(this->state.formatDesc != nullptr)
			CFRelease(this->state.formatDesc);
		const OSStatus result = this->state.hevc ?
			CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, (const uint8_t**)this->state.paramSets,
				this->state.paramLengths, sizeof(uint32_t), nullptr, &this->state.formatDesc) :
			CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, (const uint8_t**)&this->state.paramSets[UnitType::SPS],
				&this->state.paramLengths[UnitType::SPS], sizeof(uint32_t), &this->state.formatDesc);
		if(result != noErr) {
			spdlog::error("CMVideoFormatDescriptionCreateFrom{}ParameterSets() failed", this->state.hevc ? "HEVC" : "H264");
			this->state.formatDesc = nullptr;
		}
		for(uint8_t *&set : this->state.paramSets) {
			free(set);
			set = nullptr;
		}
		if(this->state.session == nullptr || (this->state.formatDesc != nullptr && VTDecompressionSessionCanAcceptFormatDescription(this->state.session, this->state.formatDesc)))
			continue;
		VTDecompressionSessionWaitForAsynchronousFrames(this->state.session);
		VTDecompressionSessionInvalidate(this->state.session);
		CFRelease(this->state.session);
		this->state.session = nullptr;
	}
}

static void onDecodeFrame(void *const userptr, void*, const OSStatus status, const VTDecodeInfoFlags infoFlags,
		const CVImageBufferRef imageBuffer, const CMTime presentationTimeStamp, const CMTime presentationDuration) {
	wivrn::apple::decoder *const decoder = (wivrn::apple::decoder*)userptr;
	if(status != noErr || imageBuffer == nullptr) {
		spdlog::error("frame decode failed with error {}", status);
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

void wivrn::apple::decoder::frame_completed(const xrt::drivers::wivrn::from_headset::feedback &feedback,
		const xrt::drivers::wivrn::to_headset::video_stream_data_shard::timing_info_t &timing_info,
		const xrt::drivers::wivrn::to_headset::video_stream_data_shard::view_info_t &view_info) {
	std::span<uint8_t> heldData = this->frameData;
	this->frameData = {};
	if(heldData.size() == 0 || this->state.formatDesc == nullptr)
		return;
	if(this->state.session != nullptr) {
		VTDecompressionSessionWaitForAsynchronousFrames(this->state.session); // ensures `this->pendingInfo` isn't in use
	} else {
		const uint32_t size[2] = {this->description.width, this->description.height};
		// const uint32_t format = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange; // VK_KHR_sampler_ycbcr_conversion seems broken in MoltenVK
		const uint32_t format = kCVPixelFormatType_32BGRA; // Available Pixel Formats - https://developer.apple.com/library/archive/qa/qa1501/_index.html
		const CFMutableDictionaryRef options =
			CFDictionaryCreateMutable(kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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
			spdlog::error("VTDecompressionSessionCreate() failed");
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
		spdlog::error("CMBlockBufferCreateWithMemoryBlock() failed");
		free(heldData.data());
		return;
	}
	CMSampleBufferRef sampleBuffer = nullptr;
	result = CMSampleBufferCreateReady(kCFAllocatorDefault, inputFrame, this->state.formatDesc, 1, 0, nullptr, 0, nullptr, &sampleBuffer);
	CFRelease(inputFrame);
	if(result != noErr) {
		spdlog::error("CMSampleBufferCreateReady() failed");
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
