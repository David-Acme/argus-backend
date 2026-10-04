#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-talk-channel.hxx"

#include <camera/camera-errors.hxx>
#include <config/config-service.hxx>
#include <feature/talk/camera-talk-session.hxx>
#include <feature/talk/talk-uplink.hxx>
#include <shared/services/camera-driver/tapo-driver.hxx>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using fake_talk::FakeTalkChannel;
using fake_talk::kCloudPassword;

namespace
{

struct LineLog
{
  std::mutex mutex;
  int opened{0};
  int closed{0};
  std::vector<size_t> packets;
  bool failOpen{false};
  int failAfter{-1};
};

class RecordingLine final : public ICameraTalkLine
{
public:
  explicit RecordingLine(std::shared_ptr<LineLog> log) : log_(std::move(log)) {}

  DriverResult open() override
  {
    std::scoped_lock lock(log_->mutex);
    ++log_->opened;
    if (log_->failOpen)
      return DriverResult::failure("digest rejected");
    return {.ok = true, .error = {}, .data = Json::Value()};
  }

  DriverResult write(std::span<const int16_t> pcm8k) override
  {
    std::scoped_lock lock(log_->mutex);
    if (log_->failAfter >= 0 && std::cmp_greater_equal(log_->packets.size(), log_->failAfter))
      return DriverResult::failure("talk channel write failed");
    log_->packets.push_back(pcm8k.size());
    return {.ok = true, .error = {}, .data = Json::Value()};
  }

  void close() override
  {
    std::scoped_lock lock(log_->mutex);
    ++log_->closed;
  }

private:
  std::shared_ptr<LineLog> log_;
};

class TalkingDriver final : public ICameraDriver
{
public:
  explicit TalkingDriver(std::shared_ptr<LineLog> log, std::string refusal = {})
      : log_(std::move(log)), refusal_(std::move(refusal))
  {
  }

  [[nodiscard]] Json::Value capabilities() const override { return {}; }
  DriverResult status() override { return DriverResult::failure("noop"); }
  DriverResult presets() override { return DriverResult::failure("noop"); }
  DriverResult move(const DriverMoveInput&) override { return DriverResult::failure("noop"); }
  DriverResult preset(const DriverPresetInput&) override { return DriverResult::failure("noop"); }
  DriverResult settings(const DriverSettingsInput&) override { return DriverResult::failure("noop"); }
  DriverResult speak(const DriverSpeakInput&) override { return DriverResult::failure("noop"); }

  TalkLineOpen talkLine() override
  {
    if (!refusal_.empty())
      return {.line = nullptr, .error = refusal_};
    return {.line = std::make_unique<RecordingLine>(log_), .error = {}};
  }

private:
  std::shared_ptr<LineLog> log_;
  std::string refusal_;
};

struct Frames
{
  std::mutex mutex;
  std::vector<Json::Value> sent;

  TalkSessionEvents events()
  {
    return {.send = [this](const Json::Value& frame) {
      std::scoped_lock lock(mutex);
      sent.push_back(frame);
    }};
  }

  std::vector<std::string> types()
  {
    std::scoped_lock lock(mutex);
    std::vector<std::string> out;
    out.reserve(sent.size());
    for (const auto& frame : sent)
      out.push_back(frame["type"].asString());
    return out;
  }

  Json::Value last(const std::string& type)
  {
    std::scoped_lock lock(mutex);
    for (const auto& frame : std::ranges::reverse_view(sent)) {
      if (frame["type"].asString() == type)
        return frame;
    }
    return {};
  }
};

TalkSessionConfig fastConfig()
{
  return {.cameraId = 7, .sampleRate = 16000, .packetMs = 40, .idleMs = 400, .maxMs = 60000, .maxQueuedMs = 1000};
}

std::vector<int16_t> tone(size_t samples)
{
  std::vector<int16_t> out(samples);
  for (size_t i = 0; i < samples; ++i)
    out[i] = static_cast<int16_t>(6000.0 * std::sin(static_cast<double>(i) * 0.2));
  return out;
}

void waitFor(const std::function<bool()>& done)
{
  for (int i = 0; i < 400 && !done(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

}

TEST_CASE("the uplink resamples to the line rate and cuts whole packets")
{
  TalkUplink uplink({.sourceRate = 16000, .packetMs = 120, .maxQueuedMs = 1000});
  CHECK(uplink.packetSamples() == 960);
  uplink.push(tone(1600));
  CHECK_FALSE(uplink.next().has_value());
  uplink.push(tone(1600));
  const auto packet = uplink.next().value_or(std::vector<int16_t>{});
  CHECK(packet.size() == 960);
  CHECK(uplink.queuedMs() < 120);
}

TEST_CASE("the uplink keeps at most its budget and drops the oldest audio")
{
  TalkUplink uplink({.sourceRate = 8000, .packetMs = 120, .maxQueuedMs = 240});
  uplink.push(tone(8000));
  CHECK(uplink.queuedMs() == 240);
  CHECK(uplink.droppedMs() == 760);
}

TEST_CASE("a talk frame is PCM16 little endian behind a four byte header")
{
  const std::vector<uint8_t> frame{0xA8, 0x01, 0x00, 0x00, 0x34, 0x12, 0xFF, 0xFF};
  const std::vector<int16_t> values = talk_frame::parse(frame).value_or(std::vector<int16_t>{});
  REQUIRE(values.size() == 2);
  CHECK(values[0] == 0x1234);
  CHECK(values[1] == -1);
  CHECK_FALSE(talk_frame::parse(std::vector<uint8_t>{0xA7, 0x01, 0, 0, 1, 2}).has_value());
  CHECK_FALSE(talk_frame::parse(std::vector<uint8_t>{0xA8, 0x01, 0, 0, 1}).has_value());
  CHECK_FALSE(talk_frame::parse(std::vector<uint8_t>{0xA8, 0x02, 0, 0, 1, 2}).has_value());
}

TEST_CASE("a session announces itself, paces the line and closes it when stopped")
{
  auto log = std::make_shared<LineLog>();
  Frames frames;
  CameraTalkSession session(fastConfig(), std::make_shared<TalkingDriver>(log), frames.events());
  session.start();
  waitFor([&] { return !frames.last("camera:talk:ready").isNull(); });
  CHECK(frames.last("camera:talk:ready")["payload"]["packetMs"].asInt() == 40);

  const auto started = std::chrono::steady_clock::now();
  session.push(tone(6800));
  waitFor([&] {
    std::scoped_lock lock(log->mutex);
    return log->packets.size() >= 10;
  });
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed >= std::chrono::milliseconds(300));
  session.stop("stopped");
  session.join();

  std::scoped_lock lock(log->mutex);
  CHECK(log->packets.size() == 10);
  for (const auto size : log->packets)
    CHECK(size == 320);
  CHECK(log->opened == 1);
  CHECK(log->closed == 1);
  CHECK(frames.last("camera:talk:closed")["payload"]["reason"].asString() == "stopped");
  CHECK(session.finished());
}

TEST_CASE("a silent session releases the line on its own")
{
  auto log = std::make_shared<LineLog>();
  Frames frames;
  CameraTalkSession session(fastConfig(), std::make_shared<TalkingDriver>(log), frames.events());
  session.start();
  waitFor([&] { return session.finished(); });
  CHECK(session.finished());
  CHECK(frames.last("camera:talk:closed")["payload"]["reason"].asString() == "idle");
  std::scoped_lock lock(log->mutex);
  CHECK(log->closed == 1);
}

TEST_CASE("a busy line is a conflict and a refused session is a bad gateway")
{
  auto log = std::make_shared<LineLog>();
  {
    Frames frames;
    CameraTalkSession session(
        fastConfig(),
        std::make_shared<TalkingDriver>(log, std::string(CameraErrors::TalkLineBusy.message)),
        frames.events());
    session.start();
    session.join();
    CHECK(frames.last("camera:talk:start_error")["status"].asInt() == 409);
    CHECK(frames.last("camera:talk:ready").isNull());
  }
  {
    log->failOpen = true;
    Frames frames;
    CameraTalkSession session(fastConfig(), std::make_shared<TalkingDriver>(log), frames.events());
    session.start();
    session.join();
    const auto refusal = frames.last("camera:talk:start_error");
    CHECK(refusal["status"].asInt() == 502);
    CHECK(refusal["error"].asString() == "digest rejected");
    std::scoped_lock lock(log->mutex);
    CHECK(log->closed == 1);
  }
}

TEST_CASE("a line that stops taking audio ends the session as lost")
{
  auto log = std::make_shared<LineLog>();
  log->failAfter = 2;
  Frames frames;
  CameraTalkSession session(fastConfig(), std::make_shared<TalkingDriver>(log), frames.events());
  session.start();
  waitFor([&] { return !frames.last("camera:talk:ready").isNull(); });
  session.push(tone(6400));
  waitFor([&] { return session.finished(); });
  CHECK(frames.last("camera:talk:closed")["payload"]["reason"].asString() == "line_lost");
}

TEST_CASE("a Tapo camera carries a live session to its talk channel and releases it")
{
  FakeTalkChannel channel(FakeTalkChannel::Mode::AcceptReference);
  const std::string path = "camera-talk-test.toml";
  {
    std::ofstream config(path);
    config << "[tapo]\nmedia_port = " << channel.port()
           << "\ncontrol_port = 1\nconnect_timeout_ms = 2000\nrequest_timeout_ms = 2000\n"
              "talk_mode = \"aec\"\ntalk_framing = \"none\"\n";
  }
  ConfigService::load(path);

  CameraSchema camera;
  camera.id = 9;
  camera.ip = "127.0.0.1";
  camera.model = "C225";
  camera.cloudPassword = kCloudPassword;
  camera.config = R"({"catalogId":"tapo-c225"})";
  auto driver = std::make_shared<TapoDriver>(camera);
  CHECK(driver->capabilities()["talk"].asBool());
  CHECK(driver->capabilities()["ptz"].asBool());

  Frames frames;
  {
    CameraTalkSession session({.cameraId = 9,
                               .sampleRate = 16000,
                               .packetMs = 120,
                               .idleMs = 5000,
                               .maxMs = 60000,
                               .maxQueuedMs = 1000},
                              driver,
                              frames.events());
    session.start();
    waitFor([&] { return !frames.last("camera:talk:ready").isNull(); });
    REQUIRE_FALSE(frames.last("camera:talk:ready").isNull());

    const auto busy = driver->talkLine();
    CHECK(busy.line == nullptr);
    CHECK(busy.error == CameraErrors::TalkLineBusy.message);

    session.push(tone(16000 * 7 / 10));
    waitFor([&] { return channel.audioParts().size() >= 5; });
    session.stop("stopped");
    session.join();
  }
  waitFor([&] { return !channel.stopPlaintext().empty(); });
  CHECK(channel.audioParts().size() == 5);
  CHECK(channel.stopPlaintext().find("\"stop\":\"null\"") != std::string::npos);
  CHECK(frames.last("camera:talk:closed")["payload"]["reason"].asString() == "stopped");

  auto again = driver->talkLine();
  CHECK(again.line != nullptr);
  std::remove(path.c_str());
}
