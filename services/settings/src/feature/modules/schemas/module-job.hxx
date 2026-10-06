#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class JobState : std::uint8_t
{
  Queued,
  Checking,
  Downloading,
  Verifying,
  Activating,
  HealthCheck,
  Removing,
  Purging,
  Done,
  Paused,
  Failed,
  Cancelled
};

constexpr std::string_view jobStateToString(JobState state)
{
  switch (state) {
  case JobState::Queued: return "queued";
  case JobState::Checking: return "checking";
  case JobState::Downloading: return "downloading";
  case JobState::Verifying: return "verifying";
  case JobState::Activating: return "activating";
  case JobState::HealthCheck: return "health_check";
  case JobState::Removing: return "removing";
  case JobState::Purging: return "purging";
  case JobState::Done: return "done";
  case JobState::Paused: return "paused";
  case JobState::Failed: return "failed";
  case JobState::Cancelled: return "cancelled";
  }
  return "failed";
}

constexpr std::optional<JobState> jobStateFromString(std::string_view text)
{
  for (const auto state : {JobState::Queued, JobState::Checking, JobState::Downloading, JobState::Verifying,
                           JobState::Activating, JobState::HealthCheck, JobState::Removing, JobState::Purging,
                           JobState::Done, JobState::Paused,
                           JobState::Failed, JobState::Cancelled})
    if (jobStateToString(state) == text)
      return state;
  return std::nullopt;
}

constexpr bool jobTerminal(JobState state)
{
  return state == JobState::Done || state == JobState::Failed || state == JobState::Cancelled;
}

constexpr bool jobRunning(JobState state)
{
  return state == JobState::Checking || state == JobState::Downloading || state == JobState::Verifying ||
         state == JobState::Activating || state == JobState::HealthCheck || state == JobState::Removing ||
         state == JobState::Purging;
}

enum class JobKind : std::uint8_t
{
  Install,
  Uninstall,
  Purge
};

constexpr std::string_view jobKindToString(JobKind kind)
{
  switch (kind) {
  case JobKind::Install: return "install";
  case JobKind::Uninstall: return "uninstall";
  case JobKind::Purge: return "purge";
  }
  return "install";
}

constexpr std::optional<JobKind> jobKindFromString(std::string_view text)
{
  for (const auto kind : {JobKind::Install, JobKind::Uninstall, JobKind::Purge})
    if (jobKindToString(kind) == text)
      return kind;
  return std::nullopt;
}

struct ModuleJobSchema
{
  std::int64_t id{0};
  std::string moduleId;
  JobKind kind{JobKind::Install};
  std::string owner;
  JobState state{JobState::Queued};
  std::string reason;
  std::int64_t bytesDone{0};
  std::int64_t bytesTotal{0};
  std::int64_t requestedBy{0};
  std::int64_t createdAt{0};
  std::int64_t updatedAt{0};
  std::int64_t stateSince{0};
};
