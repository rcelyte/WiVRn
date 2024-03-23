#include "audio.h"
#include "spdlog/spdlog.h"

wivrn::apple::audio::audio(const xrt::drivers::wivrn::to_headset::audio_stream_description &desc, wivrn_session &session, xr::instance &instance) {
	AudioStreamBasicDescription streamDesc = {
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		.mFramesPerPacket = 1,
		.mBitsPerChannel = sizeof(uint16_t) * 8,
	};
	if(const auto device = desc.speaker) {
		streamDesc.mSampleRate = device->sample_rate;
		streamDesc.mBytesPerFrame = streamDesc.mBytesPerPacket = sizeof(uint16_t) * device->num_channels;
		streamDesc.mChannelsPerFrame = device->num_channels;
		const OSStatus result = AudioQueueNewOutput(&streamDesc, [](void*, const AudioQueueRef queue, const AudioQueueBufferRef buffer) {
			AudioQueueFreeBuffer(queue, buffer);
		}, nullptr, CFRunLoopGetMain(), kCFRunLoopCommonModes, 0, &this->output);
		if(result != noErr)
			spdlog::error("AudioQueueNewOutput() failed");
		if(AudioQueueStart(this->output, nullptr) != noErr) {
			AudioQueueDispose(this->output, true);
			this->output = nullptr;
			spdlog::warn("Speaker stream failed to start");
		}
	}
	// TODO: input
	/*if(const auto device = desc.microphone) {
		streamDesc.mSampleRate = device->sample_rate;
		streamDesc.mBytesPerFrame = streamDesc.mBytesPerPacket = sizeof(uint16_t) * device->num_channels;
		streamDesc.mChannelsPerFrame = device->num_channels;
		AudioQueueNewInput(&streamDesc);
	}*/
}

wivrn::apple::audio::~audio() {
	if(this->input != nullptr)
		AudioQueueDispose(this->input, true);
	if(this->output != nullptr)
		AudioQueueDispose(this->output, true);
}

void wivrn::apple::audio::operator()(xrt::drivers::wivrn::audio_data &&data) {
	if(this->output == nullptr)
		return;
	AudioQueueBufferRef buffer = nullptr;
	const OSStatus result = AudioQueueAllocateBuffer(this->output, (uint32_t)data.payload.size(), &buffer);
	if(result != noErr)
		return;
	memcpy(buffer->mAudioData, data.payload.data(), buffer->mAudioDataBytesCapacity);
	buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
	AudioQueueEnqueueBuffer(this->output, buffer, 0, nullptr);
	// TODO: data.timestamp -> AudioQueueEnqueueBufferWithParameters()
	// TODO: trim frames if latency builds up
}

void wivrn::apple::audio::get_audio_description(xrt::drivers::wivrn::from_headset::headset_info_packet& info) {
	info.speaker = { // TODO: get actual channel count + sample rate
		.num_channels = 2,
		.sample_rate = 48000,
	};
	// TODO: microphone params
}

void wivrn::apple::audio::request_mic_permission() {
	// TODO
}
