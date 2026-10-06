#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class ComponentSource : std::uint8_t
{
  Download,
  Provisioned
};

enum class ComponentState : std::uint8_t
{
  Installed,
  Missing,
  Installing,
  Failed,
  HostOnly
};

constexpr std::string_view componentSourceToString(ComponentSource source)
{
  return source == ComponentSource::Download ? "download" : "provisioned";
}

constexpr std::optional<ComponentSource> componentSourceFromString(std::string_view text)
{
  if (text == "download")
    return ComponentSource::Download;
  if (text == "provisioned")
    return ComponentSource::Provisioned;
  return std::nullopt;
}

constexpr std::string_view componentStateToString(ComponentState state)
{
  switch (state) {
  case ComponentState::Installed: return "installed";
  case ComponentState::Missing: return "missing";
  case ComponentState::Installing: return "installing";
  case ComponentState::Failed: return "failed";
  case ComponentState::HostOnly: return "host_only";
  }
  return "missing";
}

constexpr std::optional<ComponentState> componentStateFromString(std::string_view text)
{
  if (text == "installed")
    return ComponentState::Installed;
  if (text == "missing")
    return ComponentState::Missing;
  if (text == "installing")
    return ComponentState::Installing;
  if (text == "failed")
    return ComponentState::Failed;
  if (text == "host_only")
    return ComponentState::HostOnly;
  return std::nullopt;
}

struct ComponentFile
{
  std::string path;
  std::string url;
  std::int64_t sizeBytes{0};
  std::string sha256;
};

struct ComponentSpec
{
  std::string id;
  ComponentSource source{ComponentSource::Provisioned};
  std::vector<ComponentFile> files;
  std::string hostCommand;

  [[nodiscard]] std::int64_t totalBytes() const
  {
    std::int64_t total = 0;
    for (const auto& file : files)
      total += file.sizeBytes;
    return total;
  }
};

struct ComponentStatus
{
  std::string id;
  ComponentState state{ComponentState::Missing};
  std::int64_t bytesPresent{0};
  std::int64_t bytesTotal{0};
  bool ready{false};
  std::string hostCommand;
  std::string reason;
};

struct ModuleLocalized
{
  std::string es;
  std::string en;
};

struct ModuleIntroLine
{
  std::string what;
  std::vector<std::string> examples;
};

struct ModuleIntroLocalized
{
  ModuleIntroLine es;
  ModuleIntroLine en;
};

struct ModuleEnabled
{
  std::string id;
  bool enabled{false};
  std::string lifecycle{};
  std::int64_t dataPurgedAt{0};
  std::vector<std::string> roles{};
  ModuleLocalized name{};
  ModuleLocalized summary{};
  ModuleIntroLocalized intro{};
  std::string kind{};
};

struct OwnerCatalogReply
{
  std::string modulesJson;
  std::int64_t version{0};
};

struct ModuleStatesReply
{
  std::vector<ModuleEnabled> modules;
  std::int64_t version{0};
  bool settled{false};
  std::string epoch{};
};

struct ModuleDataItem
{
  std::string kind;
  std::int64_t count{0};
};

struct ModuleDataSummary
{
  std::vector<ModuleDataItem> items;
  std::int64_t bytes{0};

  [[nodiscard]] bool empty() const
  {
    if (bytes > 0)
      return false;
    for (const auto& item : items)
      if (item.count > 0)
        return false;
    return true;
  }
};

struct ImpactRoleHolder
{
  std::int64_t userId{0};
  std::string name;
  std::string lastName;
  std::string role;
  bool isActive{true};
};

struct PendingInvitation
{
  std::int64_t id{0};
  std::string role;
  std::int64_t createdBy{0};
  std::string createdByName;
  std::int64_t expiresAt{0};
};

struct ModuleImpactReport
{
  std::vector<ModuleDataItem> stops;
  std::vector<ImpactRoleHolder> roleHolders;
  std::vector<PendingInvitation> invitations;
};

struct RoleReassignment
{
  std::int64_t userId{0};
  std::string role;
};

struct RoleReassignmentBatch
{
  std::int64_t actorUserId{0};
  std::vector<RoleReassignment> reassignments;
};

enum class ReassignStatus : std::uint8_t
{
  Applied,
  Refused,
  Failed
};

struct RoleReassignmentOutcome
{
  ReassignStatus status{ReassignStatus::Failed};
  std::int32_t applied{0};
  std::int64_t failedUserId{0};
  std::string reason;
};

struct ModuleRequestInput
{
  std::string moduleId;
  ModuleLocalized moduleName;
  std::int64_t userId{0};
  std::string day;
};

struct ModuleRequestOutcome
{
  std::int32_t notified{0};
  bool duplicate{false};
};

struct ModuleDataPurge
{
  bool purged{false};
  std::string reason;
};

enum class PinVerdict : std::uint8_t
{
  NoPin,
  Accepted,
  Required,
  Invalid,
  Locked
};
