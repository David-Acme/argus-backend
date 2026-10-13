#pragma once

#include <auth/module-snapshot.hxx>
#include <auth/user-role.hxx>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <routes/remote-link.hxx>
#include <shared/services/room/room-manager.hxx>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace user_context
{
struct BuildInput
{
  int64_t userId;
  UserRole role;
  const ModuleSnapshot& modules;
  const Json::Value& ownerCatalog;
  const std::optional<routes::RemoteLinkState>& remote;
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

  void remember(int64_t userId, UserRole role) const;
  void deliverLocal(const Conn& conn, const ContextDelivery& delivery) const;
  void modulesChanged() const;
  void userChanged(int64_t userId, UserRole role) const;

  void setRemote(std::optional<routes::RemoteLinkState> remote) const;
  [[nodiscard]] std::optional<routes::RemoteLinkState> remote() const;
  void remoteChanged() const;
  void deliverRemote(const Conn& conn, const ModuleSnapshot& modules) const;

  [[nodiscard]] std::optional<UserRole> roleOf(int64_t userId) const;

private:
  [[nodiscard]] std::shared_ptr<const CatalogFetch> fetch() const;

  mutable std::mutex mutex_;
  std::shared_ptr<const CatalogFetch> fetch_;
  mutable std::unordered_map<int64_t, UserRole> roles_;
  mutable std::optional<routes::RemoteLinkState> remote_;
};

UserContextService& userContext();
