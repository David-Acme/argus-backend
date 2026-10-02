#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/actions/audio-capture.hxx>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

constexpr int kRate = 16000;

std::filesystem::path scratchDir()
{
  const auto dir = std::filesystem::temp_directory_path() /
                   ("argus-capture-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return dir;
}

void writeVoice(const std::filesystem::path& path)
{
  std::vector<int16_t> samples(static_cast<size_t>(kRate) * 3 / 2);
  for (size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<int16_t>(
        12000.0 * std::sin(2.0 * std::numbers::pi * 300.0 *
                           static_cast<double>(i) / kRate));
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
}

void installLiveStreamFfmpeg(const std::filesystem::path& dir)
{
  const auto voice = dir / "voice.raw";
  writeVoice(voice);
  const auto script = dir / "ffmpeg";
  {
    std::ofstream out(script);
    out << "#!/bin/sh\ncat '" << voice.string() << "'\nexec cat /dev/zero\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  const char* path = std::getenv("PATH");
  const std::string searched =
      dir.string() + ":" + (path != nullptr ? path : "/usr/bin:/bin");
  ::setenv("PATH", searched.c_str(), 1);
}

}

TEST_CASE("a capture that endpoints on a live stream keeps what it heard")
{
  const auto dir = scratchDir();
  installLiveStreamFfmpeg(dir);

  const auto captured = audio_capture::capture(
      {.url = "rtsp://127.0.0.1:8554/camera-1-sub", .seconds = 8,
       .endpoint = true});

  CHECK(captured.ok);
  CHECK(captured.status == AudioCaptureStatus::Success);
  CHECK(captured.speechDetected);
  CHECK(captured.endpointed);
  CHECK(captured.samples.size() >= static_cast<size_t>(kRate));
  CHECK(captured.error.empty());

  std::filesystem::remove_all(dir);
}
