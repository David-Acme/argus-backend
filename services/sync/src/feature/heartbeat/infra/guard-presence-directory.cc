#include "guard-presence-directory.hxx"

#include <feature/heartbeat/services/heartbeat-policy.hxx>

#include <string>
#include <utility>

namespace
{
std::string presenceName(argus::guard::v1::PresenceState state)
{
  switch (state) {
    case argus::guard::v1::PRESENCE_STATE_HOME:
      return std::string(heartbeat::kPresenceHome);
    case argus::guard::v1::PRESENCE_STATE_AWAY:
      return std::string(heartbeat::kPresenceAway);
    default:
      return std::string(heartbeat::kPresenceUnknown);
  }
}
}

std::vector<PresenceEntry>
heartbeat::presenceEntriesOf(const argus::guard::v1::ListPresenceResponse& response)
{
  std::vector<PresenceEntry> entries;
  entries.reserve(static_cast<size_t>(response.users_size()));
  for (const auto& user : response.users())
    entries.push_back(
        {.userId = user.user_id(), .overall = presenceName(user.overall()), .since = user.since()});
  return entries;
}

GuardPresenceDirectory::GuardPresenceDirectory(std::shared_ptr<const GuardPresenceClient> client)
    : client_(std::move(client))
{
}

std::optional<std::vector<PresenceEntry>> GuardPresenceDirectory::list() const
{
  if (!client_)
    return std::nullopt;
  const auto response = client_->listPresence({});
  if (!response)
    return std::nullopt;
  return heartbeat::presenceEntriesOf(*response);
}
