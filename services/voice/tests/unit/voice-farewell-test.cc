#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/voice/farewell-lines.hxx>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct ScriptedTts final : IVoiceTts
{
  std::atomic<int> calls{0};
  std::atomic<bool> down{false};

  [[nodiscard]] float defaultSpeed(std::stop_token = {}) const override { return 1.0F; }
  [[nodiscard]] int sampleRate(std::stop_token = {}) const override { return 24000; }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    ++calls;
    if (down.load())
      throw std::runtime_error("tts down");
    input.onChunk(std::vector<float>(2400, 0.0F));
    input.onChunk(std::vector<float>(2400, 0.5F));
    input.onChunk(std::vector<float>(2400, -0.5F));
    input.onChunk(std::vector<float>(2400, 0.0F));
  }
};

}

TEST_CASE("the revocation cause picks the line, anything unknown is a closed session")
{
  CHECK(farewellReasonOf("accountDisabled") == FarewellReason::AccountDisabled);
  CHECK(farewellReasonOf("revokedByOwner") == FarewellReason::ClosedByOwner);
  CHECK(farewellReasonOf("logout") == FarewellReason::SessionClosed);
  CHECK(farewellReasonOf("refreshTokenReuse") == FarewellReason::SessionClosed);
  CHECK(farewellReasonOf("") == FarewellReason::SessionClosed);
  CHECK(farewellLine({.lang = VoiceLang::Es, .reason = FarewellReason::SessionClosed}) ==
        "Tu sesión se ha cerrado, cuelgo.");
  CHECK(farewellLine({.lang = VoiceLang::En, .reason = FarewellReason::SessionClosed}) ==
        "Your session was closed, I'm hanging up.");
  CHECK(farewellLine({.lang = VoiceLang::Es, .reason = FarewellReason::AccountDisabled}) ==
        "Tu cuenta está desactivada, cuelgo.");
  CHECK(farewellLine({.lang = VoiceLang::En, .reason = FarewellReason::ClosedByOwner}) ==
        "This session was closed, I'm hanging up.");
}

TEST_CASE("the cache synthesizes every line once, at 16 kHz, and answers from memory")
{
  ScriptedTts tts;
  FarewellCache cache(tts);
  CHECK(cache.audio({.lang = VoiceLang::Es, .reason = FarewellReason::SessionClosed}).pcm == nullptr);
  CHECK(cache.warmOnce({}));
  CHECK(tts.calls.load() == 6);
  const FarewellAudio es = cache.audio({.lang = VoiceLang::Es, .reason = FarewellReason::SessionClosed});
  REQUIRE(es.pcm != nullptr);
  CHECK(es.text == "Tu sesión se ha cerrado, cuelgo.");
  CHECK(es.pcm->size() > 3000);
  CHECK(es.pcm->size() < 3400 + 2 * FarewellCache::kSampleRate / 25);
  CHECK(es.pcm->front() == 0);
  CHECK(std::abs(es.pcm->at(FarewellCache::kSampleRate / 25 + 40)) > 300);
  CHECK(cache.audio({.lang = VoiceLang::System, .reason = FarewellReason::SessionClosed}).pcm == es.pcm);
  CHECK(cache.warmOnce({}));
  CHECK(tts.calls.load() == 6);
}

TEST_CASE("with TTS down the cache stays empty and keeps retrying in the background")
{
  ScriptedTts tts;
  tts.down = true;
  FarewellCache cache(tts);
  CHECK_FALSE(cache.warmOnce({}));
  CHECK(cache.audio({.lang = VoiceLang::En, .reason = FarewellReason::AccountDisabled}).pcm == nullptr);
  tts.down = false;
  CHECK(cache.warmOnce({}));
  CHECK(cache.audio({.lang = VoiceLang::En, .reason = FarewellReason::AccountDisabled}).pcm != nullptr);
}

TEST_CASE("a warming cache stops at once when it is destroyed")
{
  ScriptedTts tts;
  tts.down = true;
  const auto started = std::chrono::steady_clock::now();
  {
    FarewellCache cache(tts);
    cache.startWarming();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}
