#include "user-context.hxx"

#include <auth/capability.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/module-gate.hxx>
#include <auth/role-access.hxx>
#include <runtime/blocking-task.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-operation.hxx>
#include <text/json-util.hxx>

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <array>
#include <exception>
#include <string_view>
#include <utility>

namespace
{
constexpr std::array kAssignableRoles = {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest};

Json::Value stringList(const std::vector<std::string>& values)
{
  Json::Value list(Json::arrayValue);
  for (const auto& value : values)
    list.append(value);
  return list;
}

Json::Value localized(const ModuleText& text)
{
  Json::Value json(Json::objectValue);
  json["es"] = text.es;
  json["en"] = text.en;
  return json;
}

Json::Value introLine(const ModuleIntroText& line)
{
  Json::Value json(Json::objectValue);
  json["what"] = line.what;
  json["examples"] = stringList(line.examples);
  return json;
}

std::string kindOf(const ModuleFlag& module)
{
  if (!module.kind.empty())
    return module.kind;
  return module.id == kCoreModule ? "core" : "available";
}

std::string lifecycleOf(const ModuleFlag& module)
{
  if (!module.lifecycle.empty())
    return module.lifecycle;
  return module.enabled ? "active" : "disabled";
}

Json::Value moduleJson(const ModuleFlag& module)
{
  Json::Value json(Json::objectValue);
  json["id"] = module.id;
  json["kind"] = kindOf(module);
  json["name"] = localized(module.name);
  json["summary"] = localized(module.summary);
  Json::Value intro(Json::objectValue);
  intro["es"] = introLine(module.intro.es);
  intro["en"] = introLine(module.intro.en);
  json["intro"] = std::move(intro);
  json["roles"] = stringList(module.roles);
  json["enabled"] = module.enabled;
  json["lifecycle"] = lifecycleOf(module);
  json["dataPurgedAt"] = module.dataPurgedAt > 0 ? Json::Value(static_cast<Json::Int64>(module.dataPurgedAt))
                                                 : Json::Value(Json::nullValue);
  return json;
}

Json::Value rolesJson(const ModuleSnapshot& modules)
{
  Json::Value list(Json::arrayValue);
  for (const auto role : kAssignableRoles) {
    Json::Value entry(Json::objectValue);
    const auto module = modules.moduleOfRole(role);
    entry["id"] = userRoleToString(role);
    entry["module"] = module ? std::string(*module) : std::string(kCoreModule);
    entry["active"] = modules.roleActive(role);
    list.append(std::move(entry));
  }
  return list;
}
}

namespace user_context
{
Json::Value build(const BuildInput& input)
{
  Json::Value context(Json::objectValue);
  context["userId"] = static_cast<Json::Int64>(input.userId);
  context["role"] = userRoleToString(input.role);
  context["roleActive"] = input.modules.roleActive(input.role);
  Json::Value capabilities(Json::arrayValue);
  for (const auto capability : role_access::capabilitiesFor({.role = input.role, .modules = input.modules}))
    capabilities.append(std::string(capability));
  context["capabilities"] = std::move(capabilities);
  context["roles"] = rolesJson(input.modules);
  Json::Value modules(Json::arrayValue);
  for (const auto& module : input.modules.modules())
    modules.append(moduleJson(module));
  context["modules"] = std::move(modules);
  if (input.role == UserRole::Owner && input.ownerCatalog.isArray())
    context["ownerCatalog"] = input.ownerCatalog;
  return context;
}

Json::Value updateFrame(const Json::Value& context)
{
  SocketEmitDto frame;
  frame.operation = SyncOperation::ContextUpdate;
  frame.option = TableName::User;
  frame.obj = context;
  return frame.toJson();
}
}

void UserContextService::setCatalogFetch(CatalogFetch fetch)
{
  const std::scoped_lock lock(mutex_);
  fetch_ = fetch ? std::make_shared<const CatalogFetch>(std::move(fetch)) : nullptr;
}

std::shared_ptr<const UserContextService::CatalogFetch> UserContextService::fetch() const
{
  const std::scoped_lock lock(mutex_);
  return fetch_;
}

drogon::Task<Json::Value> UserContextService::ownerCatalogFor(UserRole role) const
{
  const auto source = fetch();
  if (role != UserRole::Owner || !source)
    co_return Json::Value(Json::nullValue);
  std::optional<std::string> raw;
  try {
    raw = co_await BlockingTask<std::optional<std::string>>([source] { return (*source)(); });
  }
  catch (const std::exception& error) {
    LOG_WARN << "Context: the owner catalog could not be read (" << error.what() << ")";
  }
  if (!raw)
    co_return Json::Value(Json::nullValue);
  Json::Value catalog = json_util::fromString(*raw);
  co_return catalog.isArray() ? catalog : Json::Value(Json::nullValue);
}

void UserContextService::deliverLocal(const Conn& conn, const ContextDelivery& delivery) const
{
  if (!conn || !conn->hasContext() || !conn->connected())
    return;
  if (!moduleGate().settled())
    return;
  const auto& ctx = conn->getContextRef<JwtContext>();
  RoomManager{}.reconcileRoleRooms(conn, {.role = ctx.role, .modules = delivery.modules});
  const Json::Value context = user_context::build(
      {.userId = ctx.sub, .role = ctx.role, .modules = delivery.modules, .ownerCatalog = delivery.ownerCatalog});
  conn->sendJson(user_context::updateFrame(context));
}

void UserContextService::modulesChanged() const
{
  if (!drogon::app().isRunning())
    return;
  drogon::async_run([this]() -> drogon::Task<> {
    const Json::Value catalog = co_await ownerCatalogFor(UserRole::Owner);
    const auto modules = std::make_shared<const ModuleSnapshot>(moduleGate().snapshot());
    RoomManager{}.forEachConnection([this, modules, catalog](const Conn& conn) {
      deliverLocal(conn, {.modules = *modules, .ownerCatalog = catalog});
    });
  });
}

void UserContextService::userChanged(int64_t userId, UserRole role) const
{
  if (!drogon::app().isRunning())
    return;
  drogon::async_run([this, userId, role]() -> drogon::Task<> {
    const Json::Value catalog = co_await ownerCatalogFor(role);
    const auto modules = std::make_shared<const ModuleSnapshot>(moduleGate().snapshot());
    RoomManager{}.forEachUserConnection(userId, [this, modules, catalog](const Conn& conn) {
      deliverLocal(conn, {.modules = *modules, .ownerCatalog = catalog});
    });
  });
}

UserContextService& userContext()
{
  static UserContextService service;
  return service;
}
