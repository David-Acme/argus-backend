#pragma once

#include <auth/user-role.hxx>
#include <config/guard-config.hxx>
#include <feature/guard/vocabulary/guard-danger.hxx>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

enum class TamperAction : uint8_t
{
  Ignore = 0,
  Alarm,
  LensNotice
};

struct TamperWindow
{
  TamperAction action{TamperAction::Ignore};
  int64_t afterS{0};
};

struct TamperWindowInput
{
  std::string_view status;
  bool sudden{false};
  std::string_view link;
  bool protect{false};
  const TamperWindows& windows;
  int64_t homeS{300};
  int64_t offlineHomeS{240};
};

struct TamperReadingRef
{
  int64_t cameraId{0};
  int64_t environmentId{0};
  int64_t onsetAt{0};
};

struct TamperGroup
{
  int64_t environmentId{0};
  std::vector<int64_t> cameraIds;
  int64_t firstOnset{0};
};

struct GroupDangerInput
{
  bool protect{false};
  bool anyoneHome{true};
};

enum class SeenVerdict : uint8_t
{
  None = 0,
  HomeNotice,
  Lowered,
  KeptCritical,
  GuestNamed
};

struct SeenInput
{
  bool found{false};
  UserRole role{UserRole::Unknown};
  bool onDuty{false};
  bool personAway{false};
  bool protect{false};
};

struct AskInput
{
  bool protect{false};
  SeenVerdict seen{SeenVerdict::None};
  bool anyoneHome{false};
};

struct SightingShareInput
{
  double firstShare{0.0};
  double lastShare{0.0};
  double nearShare{0.12};
};

namespace guard_tamper
{
[[nodiscard]] bool indicating(std::string_view status, bool sudden);
[[nodiscard]] TamperWindow windowFor(const TamperWindowInput& input);
[[nodiscard]] std::vector<TamperGroup> groupsOf(std::span<const TamperReadingRef> readings, int64_t windowS);
[[nodiscard]] GuardDanger groupDanger(const GroupDangerInput& input);
[[nodiscard]] SeenVerdict seenVerdict(const SeenInput& input);
[[nodiscard]] bool shouldAsk(const AskInput& input);
[[nodiscard]] bool approachingLens(const SightingShareInput& input);
}
