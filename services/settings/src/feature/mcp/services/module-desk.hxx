#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>
#include <vector>

enum class ModuleState : std::uint8_t
{
  Active,
  Off,
  Installing,
  ComingSoon
};

struct ModuleCard
{
  std::string id;
  std::string name;
  std::string summary;
  std::string what;
  std::vector<std::string> examples;
  ModuleState state{ModuleState::Off};
  double progress{0.0};
  std::int64_t sizeBytes{0};
  bool hasData{false};
};

struct DeskLookup
{
  std::string moduleId;
  std::string lang;
};

struct DeskCommand
{
  std::string moduleId;
  std::int64_t userId{0};
  std::string lang;
};

enum class EnableKind : std::uint8_t
{
  Started,
  AlreadyActive,
  ComingSoon,
  HardwareInsufficient,
  JobRunning,
  Unknown,
  Unavailable
};

struct EnableOutcome
{
  EnableKind kind{EnableKind::Unavailable};
  std::string detail;
};

enum class DisableKind : std::uint8_t
{
  Disabled,
  Refused,
  Unknown,
  Unavailable
};

struct DisableOutcome
{
  DisableKind kind{DisableKind::Unavailable};
  std::string detail;
};

enum class RequestKind : std::uint8_t
{
  Requested,
  Duplicate,
  AlreadyActive,
  ComingSoon,
  Unknown,
  Unavailable
};

struct RequestOutcome
{
  RequestKind kind{RequestKind::Unavailable};
};

struct ImpactStop
{
  std::string kind;
  std::optional<std::int64_t> count;
};

struct ImpactHolder
{
  std::string name;
  std::string role;
};

struct ImpactInvitation
{
  std::string role;
  std::string invitedBy;
};

struct ImpactNote
{
  std::string spanish;
  std::string english;
};

struct ModuleImpact
{
  bool known{true};
  bool allowed{true};
  std::string refusal;
  std::string refusalCode;
  std::vector<ImpactStop> stops;
  std::vector<ImpactHolder> holders;
  std::vector<ImpactInvitation> invitations;
  std::vector<ImpactNote> keepsRunning;
};

class ModuleDesk
{
public:
  ModuleDesk() = default;
  virtual ~ModuleDesk() = default;
  ModuleDesk(const ModuleDesk&) = delete;
  ModuleDesk& operator=(const ModuleDesk&) = delete;

  [[nodiscard]] virtual drogon::Task<std::vector<ModuleCard>> list(const std::string& lang) = 0;
  [[nodiscard]] virtual drogon::Task<std::optional<ModuleCard>> find(const DeskLookup& lookup) = 0;
  [[nodiscard]] virtual drogon::Task<EnableOutcome> enable(const DeskCommand& command) = 0;
  [[nodiscard]] virtual drogon::Task<ModuleImpact> impact(const DeskLookup& lookup) = 0;
  [[nodiscard]] virtual drogon::Task<DisableOutcome> disable(const DeskCommand& command) = 0;
  [[nodiscard]] virtual drogon::Task<RequestOutcome> request(const DeskCommand& command) = 0;
};
