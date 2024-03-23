#include "wivrn_packets.h"
#include <AudioToolbox/AudioQueue.h>

class wivrn_session;

namespace xr
{
class instance;
}

namespace wivrn::apple
{
class audio
{
	AudioQueueRef output = nullptr, input = nullptr;
	void onOutput(AudioQueueRef, AudioQueueBufferRef);
public:
	audio(const audio &) = delete;
	audio & operator=(const audio&) = delete;
	audio(const xrt::drivers::wivrn::to_headset::audio_stream_description &, wivrn_session &, xr::instance &);
	~audio();

	void operator()(xrt::drivers::wivrn::audio_data &&);

	static void get_audio_description(xrt::drivers::wivrn::from_headset::headset_info_packet& info);
};
}
