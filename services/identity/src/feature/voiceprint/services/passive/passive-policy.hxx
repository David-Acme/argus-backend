#pragma once

#include <config/identity-config.hxx>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

enum class TurnVerdict : uint8_t
{
  Accepted = 0,
  MixedTurn,
  OtherSpeaker,
  Drift,
  CallFull,
  CallTainted,
  CallMismatch,
  CallClosed,
  Busy
};

struct OtherVoiceMatch
{
  int64_t userId{0};
  float score{0.0F};
};

struct TurnJudgeInput
{
  std::span<const std::vector<float>> accepted;
  std::span<const float> embedding;
  std::optional<float> halvesScore;
  std::optional<OtherVoiceMatch> bestOther;
  std::optional<float> ownScore;
};

enum class CallVerdict : uint8_t
{
  Usable = 0,
  Tainted,
  TooFewTurns,
  TooLittleSpeech,
  Inconsistent
};

struct CallJudgeInput
{
  std::span<const std::vector<float>> turns;
  float speechSeconds{0.0F};
  bool tainted{false};
};

struct CallJudgement
{
  CallVerdict verdict{CallVerdict::Tainted};
  std::vector<float> centroid;
};

struct PolicySample
{
  int64_t id{0};
  std::vector<float> embedding;
  int64_t createdAt{0};
  float speechSeconds{0.0F};
  bool ownDevice{false};
};

enum class LinkVerdict : uint8_t
{
  Link = 0,
  NoSamples,
  NotDominant,
  TooFewOccasions,
  TooFewDays,
  TooLittleSpeech,
  VoiceTaken
};

struct LinkInput
{
  std::span<const PolicySample> samples;
  std::span<const std::vector<float>> otherProfiles;
  int64_t utcOffsetSeconds{0};
};

struct LinkDecision
{
  LinkVerdict verdict{LinkVerdict::NoSamples};
  std::vector<float> centroid;
  std::vector<int64_t> members;
  int occasions{0};
  int days{0};
  float speechSeconds{0.0F};
  float dominance{0.0F};
};

enum class AdoptVerdict : uint8_t
{
  Adopt = 0,
  Weak,
  OtherBetter,
  DeviceConflicted
};

struct AdoptInput
{
  float ownScore{0.0F};
  std::optional<OtherVoiceMatch> bestOther;
  bool sharedDevice{false};
  int deviceMatched{0};
  int deviceConflicting{0};
};

struct RefreshInput
{
  std::span<const float> current;
  std::span<const std::vector<float>> reservoir;
};

struct RefreshResult
{
  bool applied{false};
  std::vector<float> centroid;
  int kept{0};
  int dropped{0};
};

struct VoiceCluster
{
  std::vector<size_t> members;
  std::vector<float> centroid;
};

class PassivePolicy
{
public:
  explicit PassivePolicy(PassiveVoiceConfig config) : config_(config) {}

  [[nodiscard]] TurnVerdict judgeTurn(const TurnJudgeInput& input) const;

  [[nodiscard]] CallJudgement judgeCall(const CallJudgeInput& input) const;

  [[nodiscard]] LinkDecision evaluateLink(const LinkInput& input) const;

  [[nodiscard]] AdoptVerdict judgeAdoption(const AdoptInput& input) const;

  [[nodiscard]] RefreshResult refresh(const RefreshInput& input) const;

  [[nodiscard]] VoiceCluster
  dominantCluster(std::span<const std::vector<float>> embeddings) const;

  [[nodiscard]] const PassiveVoiceConfig& config() const { return config_; }

private:
  PassiveVoiceConfig config_;
};
