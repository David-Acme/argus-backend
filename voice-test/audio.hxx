#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using AudioSampleCallback =
    std::function<void(const std::vector<float>& frames, double timestamp)>;

// Lists capture devices (microphones) available on the system.
// Returns pairs of (device index, device name).
std::vector<std::pair<int, std::string>> listMicrophones();

// Captures audio from the given PortAudio device index at 16 kHz mono
// when the hardware supports it; otherwise it falls back to the device's
// native rate and resamples to 16 kHz in software. Frames are delivered in
// ~32ms chunks via `onFrames`. Returns false on failure.
bool openMicrophone(int deviceIndex, AudioSampleCallback onFrames);

void closeMicrophone();

// Blocks until a key is pressed (for user flow control).
void waitForEnter();

// Streams raw mono PCM to the default output device at its native sample
// rate. `sampleRate` is the PCM sample rate; PortAudio resamples to the
// device rate, so pitch stays natural.
bool playPcm(const std::vector<float>& pcm, int sampleRate,
             const std::atomic<bool>& isInterrupted);
