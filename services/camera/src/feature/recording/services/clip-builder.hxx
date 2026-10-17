#pragma once

#include <camera/camera-clip-trigger.hxx>
#include <camera/camera-record-mode.hxx>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

struct ClipBuilderConfig
{
  int64_t preRollMs{10000};
  int64_t postRollMs{10000};
  int64_t segmentMs{120000};
  size_t preRollBytes{size_t{12} * 1024 * 1024};
  size_t maxClipBytes{size_t{96} * 1024 * 1024};
};

struct ClipChunkInput
{
  const uint8_t* payload{nullptr};
  size_t len{0};
  bool keyframe{false};
  int64_t atMs{0};
};

struct ReadyClip
{
  CameraClipTrigger trigger{CameraClipTrigger::Event};
  std::string codec;
  std::string body;
  int64_t startedAtMs{0};
  int64_t durationMs{0};
};

namespace clip_recording
{
[[nodiscard]] bool triggerAllowed(CameraRecordMode mode,
                                  CameraClipTrigger trigger);
}

class ClipBuilder
{
public:
  void configure(const ClipBuilderConfig& config);
  void setInit(const std::string& init);
  [[nodiscard]] bool hasInit() const { return hasInit_; }
  [[nodiscard]] const std::string& codec() const { return codec_; }
  [[nodiscard]] bool active() const { return active_.has_value(); }

  bool request(CameraClipTrigger trigger, int64_t atMs, int64_t epochMs);
  void push(const ClipChunkInput& input);
  [[nodiscard]] std::optional<ReadyClip> takeReady();
  void reset();

private:
  struct Chunk
  {
    std::string bytes;
    bool keyframe{false};
    int64_t atMs{0};
  };

  struct Active
  {
    CameraClipTrigger trigger{CameraClipTrigger::Event};
    int64_t anchorSteadyMs{0};
    int64_t anchorEpochMs{0};
    int64_t postRollUntilMs{0};
    int64_t firstAtMs{0};
    int64_t lastAtMs{0};
    size_t bytes{0};
    bool awaitingKeyframe{true};
    std::string body;
  };

  struct BeginInput
  {
    CameraClipTrigger trigger{CameraClipTrigger::Event};
    int64_t atMs{0};
    int64_t epochMs{0};
    int64_t preRollMs{0};
  };

  void begin(const BeginInput& input);
  void close();

  ClipBuilderConfig config_;
  std::string init_;
  std::string codec_;
  bool hasInit_{false};
  std::deque<Chunk> ring_;
  size_t ringBytes_{0};
  std::optional<Active> active_;
  std::optional<ReadyClip> ready_;
};
