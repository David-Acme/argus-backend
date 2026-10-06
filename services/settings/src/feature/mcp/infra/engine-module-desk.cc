#include "engine-module-desk.hxx"

#include <feature/modules/module-errors.hxx>

#include <errors/response-exception.hxx>

#include <algorithm>
#include <utility>

namespace
{
constexpr std::string_view kEnglish = "en";

struct Refusal
{
  const ResponseException& error;
  bool english{false};
};

bool codeIs(const ResponseException& error, const ErrorDefinition& definition)
{
  return error.errorCode() == definition.wireCode();
}

std::string refusalText(const Refusal& refusal)
{
  const auto& error = refusal.error;
  const bool english = refusal.english;
  if (codeIs(error, ModuleErrors::CoreModule))
    return english ? "the core module is always on" : "el módulo base siempre está activo";
  if (codeIs(error, ModuleErrors::RequiredBy))
    return english ? "another module that is on needs it" : "otro módulo que está activo lo necesita";
  if (codeIs(error, ModuleErrors::JobRunning))
    return english ? "it already has a job in progress" : "ya tiene un trabajo en curso";
  return error.what();
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
    else if (codeIs(error, ModuleErrors::AlreadyEnabled))
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
                      .keepsRunning = {}};
  const auto card = co_await find(lookup);
  if (!card)
    co_return answer;
  answer.known = true;
  answer.refusal = unavailableImpact(lookup.lang);
  answer.refusalCode = "impact_unavailable";
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
    outcome.detail = refusalText({.error = error, .english = command.lang == kEnglish});
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

drogon::Task<RequestOutcome> EngineModuleDesk::request(const DeskCommand&)
{
  co_return RequestOutcome{.kind = RequestKind::Unavailable};
}
