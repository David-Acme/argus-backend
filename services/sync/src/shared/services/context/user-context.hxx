#pragma once

#include <auth/module-snapshot.hxx>
#include <auth/user-role.hxx>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <shared/services/room/room-manager.hxx>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace user_context
{
struct BuildInput
{
  int64_t userId;
  UserRole role;
  const ModuleSnapshot& modules;
  const Json::Value& ownerCatalog;
};

[[nodiscard]] Json::Value build(const BuildInput& input);
[[nodiscard]] Json::Value updateFrame(const Json::Value& context);
}

struct ContextDelivery
{
  const ModuleSnapshot& modules;
  const Json::Value& ownerCatalog;
};

class UserContextService
{
public:
  using CatalogFetch = std::function<std::optional<std::string>()>;

  void setCatalogFetch(CatalogFetch fetch);

  [[nodiscard]] drogon::Task<Json::Value> ownerCatalogFor(UserRole role) const;
  [[nodiscard]] drogon::Task<Json::Value> contextFor(int64_t userId, UserRole role) const;

  void deliverLocal(const Conn& conn, const ContextDelivery& delivery) const;
  void modulesChanged() const;
  void userChanged(int64_t userId, UserRole role) const;

private:
  [[nodiscard]] std::shared_ptr<const CatalogFetch> fetch() const;

  mutable std::mutex mutex_;
  std::shared_ptr<const CatalogFetch> fetch_;
};

UserContextService& userContext();
