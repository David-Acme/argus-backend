#include "module-journal.hxx"

#include <feature/modules/schemas/module-role-move.hxx>
#include <trantor/utils/Logger.h>

#include <string>
#include <string_view>
#include <utility>

namespace
{
UserAction actionOf(ModuleAuditAction action)
{
  switch (action) {
    case ModuleAuditAction::Adopted:
    case ModuleAuditAction::InstallRequested:
      return UserAction::Create;
    case ModuleAuditAction::UninstallRequested:
    case ModuleAuditAction::Removed:
    case ModuleAuditAction::Purged:
      return UserAction::Delete;
    case ModuleAuditAction::Enabled:
    case ModuleAuditAction::Disabled:
    case ModuleAuditAction::RolledBack:
    case ModuleAuditAction::Paused:
    case ModuleAuditAction::Resumed:
    case ModuleAuditAction::Cancelled:
    case ModuleAuditAction::Failed:
      return UserAction::Update;
  }
  return UserAction::Update;
}

Json::Value moveList(const std::vector<ModuleRoleMove>& moves)
{
  Json::Value list(Json::arrayValue);
  for (const auto& move : moves) {
    Json::Value entry(Json::objectValue);
    entry["userId"] = static_cast<Json::Int64>(move.userId);
    entry["from"] = move.from;
    entry["to"] = move.to;
    list.append(std::move(entry));
  }
  return list;
}

std::string_view lifecycleAfter(ModuleAuditAction action)
{
  switch (action) {
    case ModuleAuditAction::Enabled:
      return "active";
    case ModuleAuditAction::Disabled:
    case ModuleAuditAction::RolledBack:
      return "disabled";
    case ModuleAuditAction::Purged:
      return "not_installed";
    default:
      return {};
  }
}
}

UserActionEvent module_journal::eventOf(const ModuleAuditSchema& entry)
{
  UserActionEvent event;
  event.userId = entry.userId;
  event.recordId = 0;
  event.subject = std::string(user_action_event::kModuleSubject);
  event.module = entry.moduleId;
  event.action = actionOf(entry.action);
  event.oldData = Json::Value(Json::objectValue);
  event.newData = Json::Value(Json::objectValue);
  event.newData["event"] = std::string(moduleAuditActionToString(entry.action));
  if (const auto lifecycle = lifecycleAfter(entry.action); !lifecycle.empty())
    event.newData["lifecycle"] = std::string(lifecycle);
  const auto detail = module_role_move::parse(entry.detail);
  if (!detail.data.empty())
    event.newData["detail"] = detail.data;
  if (!detail.moves.empty())
    event.newData["roleMoves"] = moveList(detail.moves);
  event.newData["at"] = static_cast<Json::Int64>(entry.createdAt);
  return event;
}

ModuleJournal::ModuleJournal(const ModuleJournalInput& input)
    : audit_(input.db),
      journal_(input.db),
      sink_(input.sink),
      pollInterval_(input.pollInterval),
      batch_(input.batch)
{
  if (!journal_.cursor())
    journal_.start(audit_.lastId());
}

ModuleJournal::~ModuleJournal()
{
  requestStop();
}

int ModuleJournal::relay()
{
  if (sink_ == nullptr)
    return 0;
  const auto from = journal_.cursor().value_or(0);
  int published = 0;
  for (const auto& entry : audit_.findAfter({.afterId = from, .limit = batch_})) {
    if (!sink_->publish({.auditId = entry.id, .event = module_journal::eventOf(entry)}))
      break;
    journal_.advance({.auditId = entry.id});
    ++published;
  }
  return published;
}

void ModuleJournal::start()
{
  if (running_.exchange(true))
    return;
  worker_ = std::jthread([this](const std::stop_token& stop) { loop(stop); });
}

void ModuleJournal::loop(const std::stop_token& stop)
{
  while (!stop.stop_requested()) {
    try {
      relay();
    }
    catch (const std::exception& error) {
      LOG_ERROR << "Modules: the action journal step failed: " << error.what();
    }
    std::unique_lock lock(waitMutex_);
    wakeUp_.wait_for(lock, stop, pollInterval_, [] { return false; });
  }
  running_ = false;
}

void ModuleJournal::requestStop()
{
  if (worker_.joinable()) {
    worker_.request_stop();
    wakeUp_.notify_all();
  }
}

bool ModuleJournal::drained() const
{
  return !running_.load();
}
