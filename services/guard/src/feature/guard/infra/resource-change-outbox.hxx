#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>
#include <string>
#include <sync/resource-change.hxx>
#include <vector>

class NatsBus;

struct ResourceChangeInput
{
  ResourceScope scope{ResourceScope::Environments};
  std::vector<int64_t> ids;
  drogon::orm::DbClient* client{nullptr};
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

  [[nodiscard]] static outbox::OutboxRepository repository();
  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr const char* kEventIdPrefix = "guard-resource";

private:
  outbox::TransactionalOutbox outbox_;
};
