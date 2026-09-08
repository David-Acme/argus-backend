#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using AudioSampleCallback =
    std::function<void(const std::vector<float>& frames, double timestamp)>;

// Lists capture devices as (device index, device name) pairs.
std::vector<std::pair<int, std::string>> listMicrophones();

// Captures at 16 kHz mono with native-rate fallback and software resampling; ~32 ms frames.
bool openMicrophone(int deviceIndex, AudioSampleCallback onFrames);

void closeMicrophone();

// Blocks until a key is pressed (for user flow control).
void waitForEnter();

// Streams raw mono PCM to the default output; PortAudio resamples to the device rate.
bool playPcm(const std::vector<float>& pcm, int sampleRate,
             const std::atomic<bool>& isInterrupted);

bool openPlayback(int sampleRate, int targetLatencyMs = 300);
bool writePlayback(const std::vector<float>& pcm,
                   const std::atomic<bool>& isInterrupted);
void flushPlayback();
void drainPlayback();
void closePlayback();
