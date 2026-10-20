#pragma once

#include <auth/user-role.hxx>
#include <feature/proximity/services/room-belief.hxx>
#include <shared/vocabulary/presence-state.hxx>
#include <shared/vocabulary/whereabouts-voice.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct WhereaboutsMember
{
  int64_t userId{0};
  std::string name;
  UserRole role{UserRole::Unknown};
  bool active{true};
};

struct WhereaboutsPresence
{
  int64_t userId{0};
  PresenceState state{PresenceState::Unknown};
  int64_t since{0};
};

struct WhereaboutsPlace
{
  int64_t id{0};
  std::string name;
};

struct WhereaboutsCamera
{
  int64_t id{0};
  std::string name;
};

struct WhereaboutsFacts
{
  std::vector<WhereaboutsMember> members;
  std::vector<RoomVerdict> verdicts;
  std::optional<std::unordered_set<int64_t>> located;
  std::vector<WhereaboutsPresence> presence;
  std::vector<WhereaboutsPlace> places;
  std::vector<WhereaboutsCamera> cameras;
  int64_t nowMs{0};
};

struct WhereaboutsAsk
{
  int64_t askerId{0};
  std::string lang;
  std::string person;
  std::string room;
};

struct WhereaboutsAnswerInput
{
  const WhereaboutsAsk& ask;
  const WhereaboutsFacts& facts;
};

struct WhereaboutsRoomLabelInput
{
  std::string_view room;
  bool english{false};
  const WhereaboutsFacts& facts;
};

enum class WhereaboutsCertainty : uint8_t
{
  Sure = 0,
  Fairly,
  Unsure
};

struct WhereaboutsCertaintyInput
{
  double confidence{0.0};
  int64_t ageMs{0};
};

namespace whereabouts
{

[[nodiscard]] std::optional<std::string> roomRoleOf(std::string_view spoken);

[[nodiscard]] std::string roomLabel(const WhereaboutsRoomLabelInput& input);

[[nodiscard]] WhereaboutsCertainty certaintyOf(const WhereaboutsCertaintyInput& input);

[[nodiscard]] WhereaboutsVoiceReply answer(const WhereaboutsAnswerInput& input);

}

struct WhereaboutsDependencies
{
  std::function<std::optional<std::vector<WhereaboutsMember>>()> members;
  std::function<drogon::Task<std::vector<RoomVerdict>>()> verdicts;
  std::function<drogon::Task<std::optional<std::unordered_set<int64_t>>>()> located;
  std::function<drogon::Task<std::vector<WhereaboutsPresence>>()> presence;
  std::function<drogon::Task<std::vector<WhereaboutsPlace>>()> places;
  std::function<std::string(int64_t)> cameraName;
  std::function<int64_t()> clockMs;
};

class WhereaboutsService
{
public:
  explicit WhereaboutsService(WhereaboutsDependencies dependencies);

  [[nodiscard]] drogon::Task<WhereaboutsVoiceReply> answer(WhereaboutsVoiceRequest request) const;

private:
  [[nodiscard]] drogon::Task<std::optional<WhereaboutsFacts>> gather() const;

  WhereaboutsDependencies dependencies_;
};
