// Audio output (audio.cpp) and the AX DSP replacement (ax.cpp).
#pragma once
#include <cstdint>

bool AudioInit();
// One AI DMA block: 32 kHz stereo, big-endian s16, right sample first in each frame.
void AudioPushBigEndianRL(const uint8_t* data, uint32_t frames);
// Frames waiting to be played, or -1 without an output device.
int64_t AudioQueuedFrames();

// Runs an AX command list (what the DSP does when AX mails it one).
void AxRunCommandList(uint32_t address);
