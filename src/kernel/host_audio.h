#pragma once

#include <cstdint>

namespace hc09
{
    // Host audio output (WASAPI shared mode, 48 kHz stereo float). The title's audio driver
    // submits 256-sample frames of 6 planar channels (FL, FR, C, LFE, SL, SR); they are mixed
    // down to stereo and queued to the default output device. Frames are dropped if the device
    // is unavailable or its buffer is full. HC09_NO_AUDIO=1 disables output.
    void SubmitAudioFrame(const float* planar, uint32_t samplesPerChannel);

    // Samples currently queued on the output device, or -1 when there is no output device (yet).
    // The audio driver uses this to pace the title's mixer to the device clock.
    int QueuedAudioSamples();
}
