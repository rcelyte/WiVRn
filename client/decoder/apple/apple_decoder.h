#pragma once
#include "wivrn_packets.h"
#include <vulkan/vulkan_raii.hpp>
#include <VideoToolbox/VTDecompressionSession.h>

struct VideoDebugWindow;
class shard_accumulator;
namespace scenes {
	class stream;
}

namespace wivrn::apple {
	struct decoder {
		struct frame_info {
			xrt::drivers::wivrn::from_headset::feedback feedback;
			xrt::drivers::wivrn::to_headset::video_stream_data_shard::timing_info_t timing_info;
			xrt::drivers::wivrn::to_headset::video_stream_data_shard::view_info_t view_info;
		};
		class blit_handle : public frame_info {
			CFTypeRef frameRef;
			vk::ImageLayout layout = vk::ImageLayout::eUndefined;
			vk::raii::Image imageHandle;
			vk::raii::ImageView viewHandle;
			public:
			vk::raii::ImageView &image_view;
			vk::Image image;
			vk::ImageLayout *current_layout;
			inline blit_handle(const struct frame_info &info, CVImageBufferRef frameRef, vk::raii::Image &&image, vk::raii::ImageView &&view);
			~blit_handle();
		};
		const xrt::drivers::wivrn::to_headset::video_stream_description::item description;
		const vk::raii::Device &device;
		const std::weak_ptr<scenes::stream> weak_scene;
		shard_accumulator *const accumulator;
		const vk::raii::Sampler frameSampler;
		struct DecodeState {
			enum UnitType {
				VPS,
				SPS,
				PPS,
				None,
			};
			bool hevc;
			std::vector<uint8_t> paramSets[UnitType::None] = {};
			CMVideoFormatDescriptionRef formatDesc;
			VTDecompressionSessionRef session;
		} state;
		uint64_t frameIndex = 0;
		std::span<uint8_t> frameData = {};
		struct frame_info pendingInfo = {};
		decoder(vk::raii::Device& device, vk::raii::PhysicalDevice &physical_device,
			const xrt::drivers::wivrn::to_headset::video_stream_description::item &description, float fps, uint8_t stream_index,
			std::weak_ptr<scenes::stream> scene, shard_accumulator *accumulator);
		decoder(const decoder&) = delete;
		decoder(decoder&&) = delete;
		~decoder();
		void push_data(std::span<std::span<const uint8_t>> shards, uint64_t frame_index, bool partial);
		void frame_completed(const xrt::drivers::wivrn::from_headset::feedback &feedback,
			const xrt::drivers::wivrn::to_headset::video_stream_data_shard::timing_info_t &timing_info,
			const xrt::drivers::wivrn::to_headset::video_stream_data_shard::view_info_t &view_info);
		inline const auto &desc() const {return description;}
		vk::Sampler sampler() {return *frameSampler;}
		vk::Extent2D image_size() {return {description.video_width, description.video_height};}
		static constexpr vk::ImageLayout framebuffer_expected_layout = vk::ImageLayout::eTransferDstOptimal;
		static constexpr vk::ImageUsageFlagBits framebuffer_usage = vk::ImageUsageFlagBits::eTransferDst;
		static std::vector<xrt::drivers::wivrn::video_codec> supported_codecs();
	};
}
