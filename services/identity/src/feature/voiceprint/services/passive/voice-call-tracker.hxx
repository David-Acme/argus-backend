#pragma once

#include <cstdint>
#include <deque>
#include <feature/voiceprint/services/passive/passive-policy.hxx>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct TrackedTurn
{
  std::string callKey;
  int64_t userId{0};
  std::string deviceHash;
  std::vector<float> embedding;
  float speechSeconds{0.0F};
  std::optional<float> halvesScore;
  std::optional<OtherVoiceMatch> bestOther;
  std::optional<float> ownScore;
  int64_t now{0};
};

struct ClosedCall
{
  std::string callKey;
  int64_t userId{0};
  std::string deviceHash;
  std::vector<std::vector<float>> turns;
  float speechSeconds{0.0F};
  bool tainted{false};
  TurnVerdict taint{TurnVerdict::Accepted};
  int rejectedTurns{0};
  int64_t startedAt{0};
  int64_t lastTurnAt{0};
};

class VoiceCallTracker
{
public:
  explicit VoiceCallTracker(PassiveVoiceConfig config);

  [[nodiscard]] TurnVerdict observe(TrackedTurn turn);

  [[nodiscard]] std::optional<ClosedCall> close(const std::string& callKey);

  [[nodiscard]] std::vector<ClosedCall> expire(int64_t now);

  [[nodiscard]] size_t open() const;

private:
  void rememberClosed(const std::string& callKey);
  [[nodiscard]] bool recentlyClosed(const std::string& callKey) const;

  PassivePolicy policy_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ClosedCall> calls_;
  std::deque<std::string> closed_;
};
