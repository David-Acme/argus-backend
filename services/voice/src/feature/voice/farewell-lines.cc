#include "farewell-lines.hxx"

#include <audio/audio-resampler.hxx>
#include <drogon/drogon.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>

namespace
{

constexpr std::array<VoiceLang, 2> kLangs{VoiceLang::Es, VoiceLang::En};
constexpr std::array<FarewellReason, 3> kReasons{FarewellReason::SessionClosed,
                                                 FarewellReason::ClosedByOwner,
                                                 FarewellReason::AccountDisabled};

constexpr float kFarewellPace = 1.12F;
constexpr int16_t kSilenceLevel = 300;
constexpr std::size_t kEdgeMargin = FarewellCache::kSampleRate / 25;

int16_t toSample(float value)
{
  return static_cast<int16_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
}

void trimSilence(std::vector<int16_t>& pcm)
{
  const auto loud = [](int16_t sample) { return sample > kSilenceLevel || sample < -kSilenceLevel; };
  const auto first = std::ranges::find_if(pcm, loud);
  if (first == pcm.end()) {
    pcm.clear();
    return;
  }
  const auto last = std::ranges::find_if(pcm.rbegin(), pcm.rend(), loud).base();
  const auto begin = static_cast<std::size_t>(first - pcm.begin());
  const auto end = static_cast<std::size_t>(last - pcm.begin());
  const std::size_t from = begin > kEdgeMargin ? begin - kEdgeMargin : 0;
  const std::size_t to = std::min(pcm.size(), end + kEdgeMargin);
  pcm = std::vector<int16_t>(pcm.begin() + static_cast<std::ptrdiff_t>(from),
                             pcm.begin() + static_cast<std::ptrdiff_t>(to));
}

}

FarewellReason farewellReasonOf(std::string_view cause)
{
  if (cause == "accountDisabled")
    return FarewellReason::AccountDisabled;
  if (cause == "revokedByOwner")
    return FarewellReason::ClosedByOwner;
  return FarewellReason::SessionClosed;
}

std::string_view farewellLine(const FarewellKey& key)
{
  const bool english = key.lang == VoiceLang::En;
  switch (key.reason) {
    case FarewellReason::AccountDisabled:
      return english ? "Your account was disabled, I'm hanging up." : "Tu cuenta está desactivada, cuelgo.";
    case FarewellReason::ClosedByOwner:
      return english ? "This session was closed, I'm hanging up." : "Han cerrado esta sesión, cuelgo.";
    case FarewellReason::SessionClosed:
      break;
  }
  return english ? "Your session was closed, I'm hanging up." : "Tu sesión se ha cerrado, cuelgo.";
}

FarewellCache::FarewellCache(IVoiceTts& tts) : tts_(tts) {}

FarewellCache::~FarewellCache()
{
  if (warmer_.joinable()) {
    warmer_.request_stop();
    warmer_.join();
  }
}

std::size_t FarewellCache::slotOf(const FarewellKey& key)
{
  const std::size_t lang = key.lang == VoiceLang::En ? 1 : 0;
  return lang * kReasons.size() + static_cast<std::size_t>(key.reason);
}

FarewellAudio FarewellCache::audio(const FarewellKey& key) const
{
  const FarewellKey normalized{.lang = key.lang == VoiceLang::En ? VoiceLang::En : VoiceLang::Es,
                               .reason = key.reason};
  std::scoped_lock lock(mutex_);
  return {.text = std::string(farewellLine(normalized)), .pcm = pcm_[slotOf(normalized)]};
}

bool FarewellCache::warmOnce(const std::stop_token& stop)
{
  bool complete = true;
  for (const VoiceLang lang : kLangs) {
    for (const FarewellReason reason : kReasons) {
      const FarewellKey key{.lang = lang, .reason = reason};
      {
        std::scoped_lock lock(mutex_);
        if (pcm_[slotOf(key)])
          continue;
      }
      if (stop.stop_requested())
        return false;
      try {
        TtsRequest request;
        request.text = std::string(farewellLine(key));
        request.lang = lang == VoiceLang::En ? TtsLang::EN : TtsLang::ES;
        request.quality = TtsQuality::Auto;
        request.speed = tts_.defaultSpeed(stop) * kFarewellPace;
        const int rate = tts_.sampleRate(stop);
        AudioResampler resampler({.sourceRate = rate > 0 ? rate : kSampleRate, .targetRate = kSampleRate});
        auto pcm = std::make_shared<std::vector<int16_t>>();
        std::vector<int16_t> raw;
        std::vector<int16_t> resampled;
        tts_.synthesizeStream({.request = request,
                               .onChunk = [&](const std::vector<float>& chunk) {
                                 raw.resize(chunk.size());
                                 std::ranges::transform(chunk, raw.begin(), toSample);
                                 resampler.processInto(raw, resampled);
                                 pcm->insert(pcm->end(), resampled.begin(), resampled.end());
                               },
                               .cancellation = stop});
        trimSilence(*pcm);
        if (pcm->empty()) {
          complete = false;
          continue;
        }
        LOG_INFO << "Voice: farewell line cached (" << voiceLangToString(lang) << ", "
                 << pcm->size() * 1000 / kSampleRate << " ms)";
        std::scoped_lock lock(mutex_);
        pcm_[slotOf(key)] = std::move(pcm);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Voice: farewell line not cached yet: " << error.what();
        complete = false;
      }
    }
  }
  return complete;
}

void FarewellCache::startWarming()
{
  warmer_ = std::jthread([this](const std::stop_token& stop) {
    std::mutex waitMutex;
    std::condition_variable_any wake;
    while (!stop.stop_requested() && !warmOnce(stop)) {
      std::unique_lock lock(waitMutex);
      wake.wait_for(lock, stop, kRetryEvery, [] { return false; });
    }
  });
}
