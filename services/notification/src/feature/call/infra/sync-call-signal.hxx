#pragma once

#include <feature/call/services/call-ports.hxx>
#include <sync/sync-client.hxx>

#include <memory>

class SyncCallSignal final : public CallSignal
{
public:
  explicit SyncCallSignal(std::shared_ptr<const SyncClient> client);

  [[nodiscard]] bool emit(const CallSignalInput& input) const override;

private:
  std::shared_ptr<const SyncClient> client_;
};
