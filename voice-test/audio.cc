#include "audio.hxx"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <portaudio.h>
#include <thread>

namespace
{

constexpr double kSampleRateIn = 16000.0;
constexpr unsigned long kFramesPerBuffer = 512;

struct CaptureState
{
  AudioSampleCallback onFrames;
  double rate{0.0};
  // Linear resampler state: fractional read position into the previous
  // buffer, so samples flow continuously across callbacks.
  double resamplePos{0.0};
  std::vector<float> resamplePrev;
};

struct PlayState
{
  const float* data{nullptr};
  size_t frames{0};
  size_t pos{0};
  const std::atomic<bool>* interrupted{nullptr};
};

CaptureState gCapture;
bool gPaInitialized = false;
PaStream* gCaptureStream = nullptr;

int captureCallback(const void* input, void* output, unsigned long framesPerBuffer,
                    const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags,
                    void* userData)
{
  auto* st = static_cast<CaptureState*>(userData);
  if (output)
    std::memset(output, 0, framesPerBuffer * sizeof(float));
  const auto* in = static_cast<const float*>(input);
  if (!in || !st->onFrames)
    return paContinue;

  if (st->rate == kSampleRateIn) {
    st->onFrames(std::vector<float>(in, in + framesPerBuffer), 0.0);
    return paContinue;
  }

  // Linear resample from the device rate down to 16 kHz, keeping continuity
  // across callbacks via resamplePos and the previous buffer tail.
  const double ratio = kSampleRateIn / st->rate;
  std::vector<float> out;
  out.reserve(static_cast<size_t>(framesPerBuffer * ratio) + 2);

  // Combined stream: previous tail + current buffer.
  std::vector<float> combined = st->resamplePrev;
  combined.insert(combined.end(), in, in + framesPerBuffer);

  while (st->resamplePos < static_cast<double>(combined.size() - 1)) {
    const double src = st->resamplePos;
    const size_t i0 = static_cast<size_t>(src);
    const size_t i1 = i0 + 1;
    const double frac = src - static_cast<double>(i0);
    out.push_back(
        static_cast<float>(combined[i0] * (1.0 - frac) + combined[i1] * frac));
    st->resamplePos += 1.0 / ratio;
  }

  // Keep the unconsumed tail (at most ~one input sample worth) and rewind
  // the fractional position accordingly.
  const size_t consumed = static_cast<size_t>(st->resamplePos);
  st->resamplePrev.assign(combined.begin() + consumed, combined.end());
  st->resamplePos -= consumed;
  st->onFrames(out, 0.0);
  return paContinue;
}

int playCallback(const void*, void* output, unsigned long framesPerBuffer,
                 const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags,
                 void* userData)
{
  auto* st = static_cast<PlayState*>(userData);
  auto* out = static_cast<float*>(output);
  if (!st->data || st->interrupted->load()) {
    std::memset(out, 0, framesPerBuffer * sizeof(float));
    return paComplete;
  }
  for (unsigned long i = 0; i < framesPerBuffer; ++i) {
    out[i] = 0.0F;
    if (st->pos < st->frames) {
      out[i] = st->data[st->pos++];
    }
  }
  if (st->pos >= st->frames)
    return paComplete;
  return paContinue;
}

} // namespace

std::vector<std::pair<int, std::string>> listMicrophones()
{
  std::vector<std::pair<int, std::string>> result;
  if (!gPaInitialized) {
    if (Pa_Initialize() != paNoError)
      return result;
    gPaInitialized = true;
  }

  int def = Pa_GetDefaultInputDevice();
  for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxInputChannels <= 0)
      continue;
    std::string name = info->name ? info->name : "(unnamed)";
    if (i == def)
      name += "  [default]";
    result.emplace_back(i, name);
  }
  return result;
}

bool openMicrophone(int deviceIndex, AudioSampleCallback onFrames)
{
  if (!gPaInitialized) {
    if (Pa_Initialize() != paNoError)
      return false;
    gPaInitialized = true;
  }

  // Close a previous capture stream before reopening.
  if (gCaptureStream) {
    Pa_StopStream(gCaptureStream);
    Pa_CloseStream(gCaptureStream);
    gCaptureStream = nullptr;
  }

  const PaDeviceInfo* info = Pa_GetDeviceInfo(deviceIndex);
  if (!info) {
    std::cerr << "Invalid device index " << deviceIndex << "\n";
    return false;
  }

  gCapture.onFrames = std::move(onFrames);
  gCapture.rate = kSampleRateIn;
  gCapture.resamplePos = 0.0;
  gCapture.resamplePrev.clear();

  PaStreamParameters inParams{};
  inParams.device = deviceIndex;
  inParams.channelCount = 1;
  inParams.sampleFormat = paFloat32;
  inParams.suggestedLatency = info->defaultLowInputLatency;

  double rate = kSampleRateIn;
  PaError err = Pa_OpenStream(&gCaptureStream, &inParams, nullptr, rate,
                              kFramesPerBuffer, paClipOff, captureCallback,
                              &gCapture);
  bool resampling = false;
  if (err != paNoError) {
    // The device may not support 16 kHz (e.g. a raw hw: device). Fall back
    // to its native rate and resample to 16 kHz in software.
    rate = info->defaultSampleRate;
    gCapture.rate = rate;
    resampling = true;
    err = Pa_OpenStream(&gCaptureStream, &inParams, nullptr, rate,
                        kFramesPerBuffer, paClipOff, captureCallback,
                        &gCapture);
  }
  if (err != paNoError) {
    std::cerr << "Failed to open capture stream: " << Pa_GetErrorText(err)
              << "\n";
    return false;
  }
  std::cout << "[mic " << deviceIndex << " @ " << static_cast<int>(rate)
            << " Hz" << (resampling ? " (resampled to 16 kHz)" : "") << "]\n";
  err = Pa_StartStream(gCaptureStream);
  if (err != paNoError) {
    std::cerr << "Failed to start capture: " << Pa_GetErrorText(err) << "\n";
    return false;
  }
  return true;
}

void closeMicrophone()
{
  if (gCaptureStream) {
    Pa_StopStream(gCaptureStream);
    Pa_CloseStream(gCaptureStream);
    gCaptureStream = nullptr;
  }
}

void waitForEnter()
{
  std::cin.ignore();
}

bool playPcm(const std::vector<float>& pcm, int sampleRate,
             const std::atomic<bool>& isInterrupted)
{
  if (!gPaInitialized) {
    if (Pa_Initialize() != paNoError)
      return false;
    gPaInitialized = true;
  }

  PaStreamParameters outParams{};
  outParams.device = Pa_GetDefaultOutputDevice();
  if (outParams.device == paNoDevice)
    return false;
  const PaDeviceInfo* info = Pa_GetDeviceInfo(outParams.device);
  outParams.channelCount = 1;
  outParams.sampleFormat = paFloat32;
  outParams.suggestedLatency = info->defaultLowOutputLatency;

  // Open the stream at the PCM's native rate (e.g. 44100 for Supertonic);
  // PortAudio resamples to the device rate. Opening at the wrong rate
  // changes pitch and makes the voice sound "effected".
  const double streamRate = sampleRate > 0 ? sampleRate : info->defaultSampleRate;

  PlayState st;
  st.data = pcm.data();
  st.frames = pcm.size();
  st.pos = 0;
  st.interrupted = &isInterrupted;

  PaStream* stream = nullptr;
  PaError err = Pa_OpenStream(&stream, nullptr, &outParams, streamRate,
                              paFramesPerBufferUnspecified, paClipOff,
                              playCallback, &st);
  if (err != paNoError)
    return false;

  err = Pa_StartStream(stream);
  if (err != paNoError)
    return false;

  while (Pa_IsStreamActive(stream) && !isInterrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (isInterrupted.load())
    Pa_AbortStream(stream);
  else
    Pa_StopStream(stream);

  Pa_CloseStream(stream);
  return true;
}
