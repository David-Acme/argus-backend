#pragma once

#include <feature/proximity/vocabulary/proximity-kinds.hxx>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct RoomBeliefConfig
{
  double weightFace{1.0};
  double weightRadarBox{0.8};
  double weightVoice{0.6};
  double weightBle{0.4};
  double nightFaceFactor{0.5};
  int64_t halfLifeMs{20000};
  double switchMargin{0.2};
  int64_t switchHoldMs{3000};
  int64_t forgetMs{300000};
};

struct RoomEvidence
{
  int64_t userId{0};
  std::string room;
  ProximityEvidenceKind kind{ProximityEvidenceKind::Ble};
  double strength{1.0};
  bool night{false};
  int64_t atMs{0};
};

struct RoomVerdict
{
  int64_t userId{0};
  std::string room;
  double confidence{0.0};
  int64_t sinceMs{0};
  int64_t updatedMs{0};
  ProximityEvidenceKind strongest{ProximityEvidenceKind::Ble};
};

struct RoomBeliefQuery
{
  int64_t userId{0};
  int64_t nowMs{0};
};

[[nodiscard]] double evidenceWeight(const RoomEvidence& evidence, const RoomBeliefConfig& config);

[[nodiscard]] double decayFactor(int64_t elapsedMs, int64_t halfLifeMs);

class RoomBelief
{
public:
  explicit RoomBelief(RoomBeliefConfig config);

  void configure(const RoomBeliefConfig& config);
  void observe(const RoomEvidence& evidence);
  [[nodiscard]] std::optional<RoomVerdict> verdict(const RoomBeliefQuery& query);
  [[nodiscard]] std::vector<RoomVerdict> all(int64_t nowMs);
  void forget(int64_t userId);

private:
  struct RoomScore
  {
    double score{0.0};
    double strongestWeight{0.0};
    ProximityEvidenceKind strongest{ProximityEvidenceKind::Ble};
  };

  struct Person
  {
    std::map<std::string, RoomScore> rooms;
    int64_t scoredAtMs{0};
    int64_t updatedMs{0};
    std::string current;
    int64_t currentSinceMs{0};
    std::string challenger;
    int64_t challengerSinceMs{0};
  };

  void decayTo(Person& person, int64_t atMs) const;
  void settle(Person& person, int64_t atMs) const;
  [[nodiscard]] std::optional<RoomVerdict> verdictOf(int64_t userId, Person& person, int64_t atMs) const;

  mutable std::mutex mutex_;
  RoomBeliefConfig config_;
  std::unordered_map<int64_t, Person> people_;
};
