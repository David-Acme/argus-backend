#include "engine-module-desk.hxx"

#include <feature/modules/module-errors.hxx>

#include <auth/user-role.hxx>

#include <errors/response-exception.hxx>

#include <algorithm>
#include <utility>

namespace
{
constexpr std::string_view kEnglish = "en";

struct Refusal
{
  std::string_view code;
  std::string_view fallback;
  bool english{false};
};

bool codeIs(const ResponseException& error, const ErrorDefinition& definition)
{
  return error.errorCode() == definition.wireCode();
}

bool isExactly(const ResponseException& error, const ErrorDefinition& definition)
{
  return codeIs(error, definition) && error.what() == definition.message;
}

std::string refusalText(const Refusal& refusal)
{
  const bool english = refusal.english;
  if (refusal.code == ModuleErrors::CoreModule.wireCode())
    return english ? "the core module is always on" : "el módulo base siempre está activo";
  if (refusal.code == ModuleErrors::RequiredBy.wireCode())
    return english ? "another module that is on needs it" : "otro módulo que está activo lo necesita";
  if (refusal.code == ModuleErrors::JobRunning.wireCode())
    return english ? "it already has a job in progress" : "ya tiene un trabajo en curso";
  return std::string(refusal.fallback);
}

ModuleState stateOf(const ModuleView& view)
{
  if (view.module != nullptr && view.module->kind == ModuleKind::ComingSoon)
    return ModuleState::ComingSoon;
  if (view.job && view.job->job.kind == JobKind::Install && view.job->job.state != JobState::Done &&
      view.job->job.state != JobState::Failed)
    return ModuleState::Installing;
  return view.enabled && view.lifecycle == ModuleLifecycle::Active ? ModuleState::Active : ModuleState::Off;
}

ModuleCard cardOf(const ModuleView& view, std::string_view lang)
{
  const bool english = lang == kEnglish;
  const auto& module = *view.module;
  const auto& intro = english ? module.intro.en : module.intro.es;
  return {.id = module.id,
          .name = module.name.in(lang),
          .summary = module.summary.in(lang),
          .what = intro.what,
          .examples = intro.examples,
          .state = stateOf(view),
          .progress = view.job ? view.job->progress : 0.0,
          .sizeBytes = view.sizeBytes,
          .hasData = view.hasData};
}

std::string_view unavailableImpact(std::string_view lang)
{
  return lang == kEnglish ? "I cannot preview what it would stop from here yet; turn it off from the app"
                          : "todavía no puedo calcular desde aquí qué detendría; apágalo desde la app";
}
}

EngineModuleDesk::EngineModuleDesk(EngineModuleDeskInput input) : engine_(input.engine) {}

drogon::Task<std::vector<ModuleCard>> EngineModuleDesk::list(const std::string& lang)
{
  std::vector<ModuleCard> cards;
  if (engine_ == nullptr)
    co_return cards;
  const auto views = co_await engine_->listAsync();
  for (const auto& view : views)
    if (view.module != nullptr)
      cards.push_back(cardOf(view, lang));
  co_return cards;
}

drogon::Task<std::optional<ModuleCard>> EngineModuleDesk::find(const DeskLookup& lookup)
{
  if (engine_ == nullptr)
    co_return std::nullopt;
  const auto views = co_await engine_->listAsync();
  const auto found = std::ranges::find_if(views, [&lookup](const ModuleView& view) {
    return view.module != nullptr && view.module->id == lookup.moduleId;
  });
  if (found == views.end())
    co_return std::nullopt;
  co_return cardOf(*found, lookup.lang);
}

drogon::Task<EnableOutcome> EngineModuleDesk::enable(const DeskCommand& command)
{
  if (engine_ == nullptr)
    co_return EnableOutcome{.kind = EnableKind::Unavailable, .detail = ""};
  EnableOutcome outcome{.kind = EnableKind::Started, .detail = ""};
  try {
    static_cast<void>(co_await engine_->installAsync({.moduleId = command.moduleId, .userId = command.userId}));
  }
  catch (const ResponseException& error) {
    outcome.detail = error.what();
    if (codeIs(error, ModuleErrors::ComingSoon))
      outcome.kind = EnableKind::ComingSoon;
    else if (codeIs(error, ModuleErrors::HardwareInsufficient))
      outcome.kind = EnableKind::HardwareInsufficient;
    else if (codeIs(error, ModuleErrors::JobRunning))
      outcome.kind = EnableKind::JobRunning;
    else if (isExactly(error, ModuleErrors::AlreadyEnabled))
      outcome.kind = EnableKind::AlreadyActive;
    else if (codeIs(error, ModuleErrors::UnknownModule))
      outcome.kind = EnableKind::Unknown;
    else
      outcome.kind = EnableKind::Unavailable;
  }
  co_return outcome;
}

drogon::Task<ModuleImpact> EngineModuleDesk::impact(const DeskLookup& lookup)
{
  ModuleImpact answer{.known = false,
                      .allowed = false,
                      .refusal = "",
                      .refusalCode = "",
                      .stops = {},
                      .holders = {},
                      .invitations = {},
                      .keepsRunning = {},
                      .unreachable = {}};
  if (engine_ == nullptr)
    co_return answer;
  std::optional<ModuleImpactView> view;
  bool unknown = false;
  try {
    view = co_await engine_->impactAsync({.moduleId = lookup.moduleId, .action = ImpactAction::Disable});
  }
  catch (const ResponseException& error) {
    unknown = codeIs(error, ModuleErrors::UnknownModule);
  }
  if (unknown)
    co_return answer;
  answer.known = true;
  if (!view) {
    answer.refusal = std::string(unavailableImpact(lookup.lang));
    answer.refusalCode = "impact_unavailable";
    co_return answer;
  }
  if (view->refusal) {
    answer.refusalCode = std::string(view->refusal->wireCode());
    answer.refusal = refusalText({.code = view->refusal->wireCode(), .fallback = view->refusal->message, .english = lookup.lang == kEnglish});
    co_return answer;
  }
  answer.allowed = true;
  answer.stops.reserve(view->stops.size());
  for (const auto& stop : view->stops)
    answer.stops.push_back({.kind = stop.kind, .count = stop.count});
  answer.holders.reserve(view->roleHolders.size());
  for (const auto& holder : view->roleHolders)
    answer.holders.push_back({.name = holder.lastName.empty() ? holder.name : holder.name + " " + holder.lastName, .role = holder.role});
  answer.invitations.reserve(view->invitations.size());
  for (const auto& invitation : view->invitations)
    answer.invitations.push_back({.role = invitation.role, .invitedBy = invitation.createdByName});
  answer.keepsRunning.reserve(view->keepsRunning.size());
  for (const auto& item : view->keepsRunning)
    answer.keepsRunning.push_back({.spanish = item.text.es, .english = item.text.en});
  answer.unreachable = view->unreachable;
  co_return answer;
}

drogon::Task<DisableOutcome> EngineModuleDesk::disable(const DeskCommand& command)
{
  if (engine_ == nullptr)
    co_return DisableOutcome{.kind = DisableKind::Unavailable, .detail = ""};
  DisableOutcome outcome{.kind = DisableKind::Disabled, .detail = ""};
  try {
    static_cast<void>(co_await engine_->disableAsync({.moduleId = command.moduleId, .userId = command.userId}));
  }
  catch (const ResponseException& error) {
    outcome.detail = refusalText({.code = error.errorCode(), .fallback = error.what(), .english = command.lang == kEnglish});
    if (codeIs(error, ModuleErrors::UnknownModule))
      outcome.kind = DisableKind::Unknown;
    else if (codeIs(error, ModuleErrors::CoreModule) || codeIs(error, ModuleErrors::RequiredBy) ||
             codeIs(error, ModuleErrors::JobRunning))
      outcome.kind = DisableKind::Refused;
    else
      outcome.kind = DisableKind::Unavailable;
  }
  co_return outcome;
}

drogon::Task<RequestOutcome> EngineModuleDesk::request(const DeskCommand& command)
{
  if (engine_ == nullptr)
    co_return RequestOutcome{.kind = RequestKind::Unavailable};
  RequestOutcome outcome{.kind = RequestKind::Requested};
  try {
    const auto view = co_await engine_->requestAsync(
        {.moduleId = command.moduleId, .userId = command.userId, .role = userRoleFromString(command.role)});
    outcome.kind = view.duplicate ? RequestKind::Duplicate : RequestKind::Requested;
  }
  catch (const ResponseException& error) {
    if (codeIs(error, ModuleErrors::ComingSoon))
      outcome.kind = RequestKind::ComingSoon;
    else if (isExactly(error, ModuleErrors::AlreadyEnabled))
      outcome.kind = RequestKind::AlreadyActive;
    else if (codeIs(error, ModuleErrors::JobRunning))
      outcome.kind = RequestKind::Installing;
    else if (codeIs(error, ModuleErrors::UnknownModule))
      outcome.kind = RequestKind::Unknown;
    else
      outcome.kind = RequestKind::Unavailable;
  }
  co_return outcome;
}
