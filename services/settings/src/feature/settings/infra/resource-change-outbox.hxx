#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>

class NatsBus;

class SettingsChangeOutbox
{
public:
  SettingsChangeOutbox(const std::shared_ptr<NatsBus>& bus,
                       outbox::ClientAccessor client);

  [[nodiscard]] drogon::Task<void> record() const;
  void recordDetached() const;

  [[nodiscard]] bool migrate() const;
  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr const char* kEventIdPrefix = "settings-resource";

private:
  outbox::ClientAccessor client_;
  outbox::TransactionalOutbox outbox_;
};
