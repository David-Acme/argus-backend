#include "audio.hxx"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <portaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <string>
#include <thread>

namespace
{

constexpr double kSampleRateIn = 16000.0;
constexpr unsigned long kFramesPerBuffer = 512;

// Windowed-sinc resampler. Linear interpolation has no anti-alias filter, so
// decimating 44.1k -> 16k folds everything above the 8k Nyquist back into the
// speech band and corrupts STT (the device now runs at 44.1k when OBS holds
// the mic via PipeWire). A band-limited sinc kernel fixes that in one pass.
constexpr int kSincHalf = 32;      // kernel half-length (64 taps)
constexpr double kSincCutoff = 7000.0;  // Hz, below the 8k output Nyquist

// Blackman window at offset j in [-kSincHalf, kSincHalf].
double sincWindow(int j)
{
  const double n = static_cast<double>(j + kSincHalf);
  const double N = static_cast<double>(2 * kSincHalf);
  return 0.42 - 0.5 * std::cos(2.0 * M_PI * n / N) +
         0.08 * std::cos(4.0 * M_PI * n / N);
}

// Band-limited sinc kernel: h(t) = 2*fc * sinc(2*fc*t) * window, with fc
// normalized in cycles per source sample and t measured in source samples.
double sincTap(double fc, double t)
{
  if (std::fabs(t) < 1e-12)
    return 2.0 * fc;
  return std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
}

struct CaptureState
{
  AudioSampleCallback onFrames;
  double rate{0.0};
  // Resampler state: fractional source position of the next output sample and
  // the history window carried over between callbacks.
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
  if (!in || !st->onFrames || framesPerBuffer == 0)
    return paContinue;
  // A stream can misreport its buffer size while the device is contended
  // (e.g. OBS holds the mic via PipeWire). Clamp to a sane maximum so the
  // reserve/insert below can never request an absurd allocation.
  const unsigned long capped = std::min<unsigned long>(framesPerBuffer,
                                                       kFramesPerBuffer * 4);

  if (st->rate == kSampleRateIn) {
    st->onFrames(std::vector<float>(in, in + capped), 0.0);
    return paContinue;
  }

  // Windowed-sinc resample from the device rate down to 16 kHz. The kernel
  // (cutoff below the 8k output Nyquist) rejects out-of-band energy before
  // decimation, so no aliasing folds noise back into the speech band.
  const double ratio = kSampleRateIn / st->rate;
  if (!(ratio > 0.0) || !(ratio < 1.0))
    return paContinue;
  std::vector<float> out;
  out.reserve(static_cast<size_t>(capped * ratio) + 2);

  // Combined stream: carried-over history + current buffer.
  std::vector<float> combined = st->resamplePrev;
  combined.insert(combined.end(), in, in + capped);

  // fc is normalized in cycles per source sample. Emit output samples while
  // the kernel's right half still fits inside the combined buffer; the left
  // half is always covered by the seeded history (resamplePos >= kSincHalf).
  const double fc = kSincCutoff / st->rate;
  const double step = 1.0 / ratio;
  while (st->resamplePos + kSincHalf <
         static_cast<double>(combined.size())) {
    const double p = st->resamplePos;
    const size_t i0 = static_cast<size_t>(p);
    const double frac = p - static_cast<double>(i0);
    double acc = 0.0;
    double wsum = 0.0;
    for (int j = -kSincHalf; j <= kSincHalf; ++j) {
      const long idx = static_cast<long>(i0) + j;
      if (idx < 0 || idx >= static_cast<long>(combined.size()))
        continue;
      const double w =
          sincTap(fc, static_cast<double>(j) - frac) * sincWindow(j);
      acc += combined[static_cast<size_t>(idx)] * w;
      wsum += w;
    }
    out.push_back(
        static_cast<float>(wsum > 1e-9 ? acc / wsum : 0.0));
    st->resamplePos += step;
  }

  // Keep the tail as history for the next callback: at least kSincHalf samples
  // before the read position so the kernel's left half stays valid, plus the
  // unconsumed samples. resamplePos is rewound relative to that window.
  const size_t start = static_cast<size_t>(st->resamplePos - kSincHalf);
  st->resamplePrev.assign(combined.begin() + start, combined.end());
  st->resamplePos -= static_cast<double>(start);
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
  if (pcm.empty())
    return false;

  // Prefer PulseAudio/PipeWire: it plays through the system sink that screen
  // recorders (OBS "Desktop Audio") capture. PortAudio-ALSA hw devices
  // bypass PipeWire and would not be recorded.
  int paErr = 0;
  pa_sample_spec ss;
  ss.format = PA_SAMPLE_FLOAT32LE;
  ss.rate = sampleRate > 0 ? static_cast<uint32_t>(sampleRate) : 44100;
  ss.channels = 1;
  pa_simple* pa = pa_simple_new(nullptr, "Argus", PA_STREAM_PLAYBACK, nullptr,
                                "voice", &ss, nullptr, nullptr, &paErr);
  if (pa) {
    const void* data = pcm.data();
    size_t bytes = pcm.size() * sizeof(float);
    if (pa_simple_write(pa, data, bytes, &paErr) == 0)
      pa_simple_drain(pa, &paErr);
    pa_simple_free(pa);
    return true;
  }

  // No PulseAudio/PipeWire server: fall back to PortAudio output devices.
  std::cerr << "[audio] pulse unavailable (" << pa_strerror(paErr)
            << "), trying PortAudio\n";
  if (!gPaInitialized) {
    if (Pa_Initialize() != paNoError)
      return false;
    gPaInitialized = true;
  }

  std::vector<int> candidates;
  const int def = Pa_GetDefaultOutputDevice();
  if (def != paNoDevice)
    candidates.push_back(def);
  for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (info && info->maxOutputChannels > 0 && i != def)
      candidates.push_back(i);
  }

  for (int device : candidates) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (!info)
      continue;

    PaStreamParameters outParams{};
    outParams.device = device;
    outParams.channelCount = 1;
    outParams.sampleFormat = paFloat32;
    outParams.suggestedLatency = info->defaultLowOutputLatency;

    const double streamRate =
        sampleRate > 0 ? sampleRate : info->defaultSampleRate;

    PlayState st;
    st.data = pcm.data();
    st.frames = pcm.size();
    st.pos = 0;
    st.interrupted = &isInterrupted;

    PaStream* stream = nullptr;
    PaError err = Pa_OpenStream(&stream, nullptr, &outParams, streamRate,
                                paFramesPerBufferUnspecified, paClipOff,
                                playCallback, &st);
    if (err != paNoError) {
      std::cerr << "[audio] open output " << device << " ("
                << (info->name ? info->name : "?")
                << ") failed: " << Pa_GetErrorText(err) << "\n";
      continue;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
      std::cerr << "[audio] start output " << device << " failed: "
                << Pa_GetErrorText(err) << "\n";
      Pa_CloseStream(stream);
      continue;
    }

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

  std::cerr << "[audio] no output device available\n";
  return false;
}
