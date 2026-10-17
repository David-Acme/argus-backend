#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>
#include <string>
#include <sync/resource-change.hxx>
#include <sync/user-change-sink.hxx>
#include <vector>

class NatsBus;

struct ResourceChangeInput
{
  ResourceScope scope{ResourceScope::Environments};
  std::vector<int64_t> ids;
  int64_t user{0};
  drogon::orm::DbClient* client{nullptr};
  ResourceDomain domain{ResourceDomain::Guard};
};

struct ResourceChangeRow
{
  std::string subject;
  std::string payload;
};

[[nodiscard]] ResourceChangeRow
resourceChangeRow(const ResourceChangeInput& input);

class ResourceChangeOutbox
{
public:
  explicit ResourceChangeOutbox(const std::shared_ptr<NatsBus>& bus);

  [[nodiscard]] drogon::Task<void>
  record(const ResourceChangeInput& input) const;

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const;

  [[nodiscard]] static outbox::OutboxRepository repository();
  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr const char* kEventIdPrefix = "guard-resource";
  static constexpr const char* kActionIdPrefix = "guard-action";

private:
  outbox::TransactionalOutbox outbox_;
};
