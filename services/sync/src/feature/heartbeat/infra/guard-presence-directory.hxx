#pragma once

#include <feature/heartbeat/infra/presence-directory.hxx>

#include <memory>

#include <guard/guard-presence-client.hxx>

class GuardPresenceDirectory : public PresenceDirectory
{
public:
  explicit GuardPresenceDirectory(std::shared_ptr<const GuardPresenceClient> client);

  [[nodiscard]] std::optional<std::vector<PresenceEntry>> list() const override;

private:
  std::shared_ptr<const GuardPresenceClient> client_;
};

namespace heartbeat
{
[[nodiscard]] std::vector<PresenceEntry>
presenceEntriesOf(const argus::guard::v1::ListPresenceResponse& response);
}
