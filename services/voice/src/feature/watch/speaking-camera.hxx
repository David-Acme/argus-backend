#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

struct SpeakingCandidate
{
  int64_t cameraId{0};
  std::string room;
  float score{0.0F};
};

struct RoomConfidence
{
  std::string room;
  double confidence{0.0};
};

struct SpeakingChoiceInput
{
  std::span<const SpeakingCandidate> candidates;
  std::span<const RoomConfidence> rooms;
  double minConfidence{0.6};
  double margin{0.2};
};

struct SpeakingChoice
{
  size_t index{0};
  bool byBelief{false};
  std::string room;
};

[[nodiscard]] std::optional<SpeakingChoice> chooseSpeakingCamera(const SpeakingChoiceInput& input);
