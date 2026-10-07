#include "turn-flow.hxx"

#include "turn-texts.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/module-command.hxx>
#include <feature/llm/services/tools/module-offer.hxx>
#include <feature/llm/services/tools/reply-claims.hxx>
#include <feature/llm/services/tools/spoken-intent.hxx>
#include <feature/llm/services/turn/tool-effects.hxx>

#include <text/iso-time.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace turn
{

namespace
{
constexpr int kMaxAttempts = 2;
constexpr std::string_view kConfirmation = "confirmation";
constexpr std::string_view kEnableTool = "modules.enable";
constexpr std::string_view kRequestTool = "modules.request";
constexpr std::string_view kInactive = "module_inactive";
constexpr std::string_view kProjectNeeded = "project_needed";
constexpr std::string_view kProjectAmbiguous = "ambiguous_project";
constexpr std::string_view kProjectUnknown = "unknown_project";
constexpr std::string_view kNoProjects = "no_projects";
constexpr std::string_view kCreateProjectTool = "project.create";
constexpr std::string_view kRemindTool = "memory.remind";
constexpr std::string_view kHeardField = "when";

const tools::ToolDescriptor* handleOf(const std::vector<tools::ToolHandle>& offered, std::string_view name)
{
  const auto found = std::ranges::find_if(offered, [name](const tools::ToolHandle& handle) { return handle->spec.name == name; });
  return found == offered.end() ? nullptr : found->get();
}

bool previewed(const tools::ToolResult& result)
{
  return result.data["needsConfirmation"].isBool() && result.data["needsConfirmation"].asBool();
}

bool performed(const tools::ToolResult& result, const tools::ToolDescriptor& tool)
{
  return result.ok && !previewed(result) && (!tool.spec.annotations.readOnly || isAppTool(result.tool));
}

bool changes(const tools::ToolResult& result, const tools::ToolDescriptor& tool)
{
  return performed(result, tool) && !isReadOnlyTool(result.tool);
}

bool shows(const tools::ToolResult& result, const tools::ToolDescriptor& tool)
{
  return performed(result, tool) && isReadOnlyTool(result.tool) && isAppTool(result.tool);
}

bool present(const Json::Value& arguments, const std::string& field)
{
  const Json::Value& value = arguments[field];
  return !value.isNull() && !(value.isString() && value.asString().empty());
}

std::vector<std::string> missingFields(const argus::mcp::ToolSpec& spec, const Candidate& candidate, const Json::Value& arguments)
{
  std::vector<std::string> fields;
  const auto add = [&](const std::string& name) {
    if (name == kConfirmation || present(arguments, name) || std::ranges::find(fields, name) != fields.end())
      return;
    fields.push_back(name);
  };
  for (const std::string& name : candidate.fill)
    add(name);
  const Json::Value& required = spec.inputSchema["required"];
  if (required.isArray())
    for (const Json::Value& name : required)
      if (name.isString())
        add(name.asString());
  return fields;
}

Finding findingOf(const tools::ToolResult& result)
{
  if (result.code == kInactive)
    return {.kind = FindingKind::Offer,
            .tool = result.tool,
            .text = result.data["facts"].isString() ? result.data["facts"].asString() : result.output};
  if (result.ok && previewed(result))
    return {.kind = FindingKind::Preview, .tool = result.tool, .text = result.output};
  Finding finding{.kind = result.ok ? FindingKind::Done : FindingKind::Refused, .tool = result.tool, .text = result.output};
  if (result.ok && result.data["readback"].isString() && result.data["readbackSentence"].isString()) {
    finding.readback = result.data["readback"].asString();
    finding.readbackSentence = result.data["readbackSentence"].asString();
  }
  return finding;
}

std::string noteOf(const Finding& finding, std::string_view lang)
{
  const turn_texts::Fact fact{.tool = finding.tool, .result = finding.text};
  switch (finding.kind) {
    case FindingKind::Done:
      return turn_texts::done(lang, fact);
    case FindingKind::Refused:
      return turn_texts::refused(lang, fact);
    case FindingKind::Preview:
      return turn_texts::preview(lang, finding.text);
    case FindingKind::Offer:
      return turn_texts::offer(lang, finding.text);
    case FindingKind::Declined:
      return turn_texts::declined(lang);
    case FindingKind::Unactionable:
      return turn_texts::unactionable(lang);
  }
  return {};
}

turn_texts::Details detailsOf(const Candidate& candidate, const TurnRequest& request)
{
  constexpr std::size_t kLongestSpoken = 60;
  const Json::Value& arguments = candidate.arguments;
  turn_texts::Details details;
  for (const char* key : {"title", "name", "text", "query"}) {
    const Json::Value& value = arguments[key];
    if (value.isString() && !value.asString().empty() && value.asString().size() <= kLongestSpoken) {
      details.title = value.asString();
      break;
    }
  }
  for (const char* key : {"starts_at", "due_at"}) {
    const Json::Value& value = arguments[key];
    if (value.isString()) {
      details.when = turn_texts::spokenWhen({.iso = value.asString(), .now = request.now, .lang = request.context.lang});
      break;
    }
  }
  if (arguments["module"].isString()) {
    const std::string id = arguments["module"].asString();
    const ModuleFlag* module = moduleNamed(request.audience.modules, id);
    const bool english = request.context.lang == "en";
    details.module = module != nullptr && !(english ? module->name.en : module->name.es).empty()
                         ? (english ? module->name.en : module->name.es)
                         : id;
  }
  return details;
}

std::string_view verdictName(Verdict verdict)
{
  switch (verdict) {
    case Verdict::Act:
      return "act";
    case Verdict::Ask:
      return "ask";
    case Verdict::Choose:
      return "choose";
    case Verdict::Pass:
      return "pass";
  }
  return "pass";
}

bool affirmed(std::string_view utterance)
{
  return spoken_intent::affirms(std::string(utterance));
}

bool declining(std::string_view utterance)
{
  return module_command::declines(utterance);
}
}

class TurnFlow::Deciding
{
public:
  Deciding(const Decider& decider, const DecideInput& input) : decider_(decider), input_(input) {}

  [[nodiscard]] const std::optional<Candidate>& get() const
  {
    if (!memo_)
      memo_ = decider_.decide(input_);
    return *memo_;
  }

  [[nodiscard]] const DecideInput& input() const { return input_; }

private:
  const Decider& decider_;
  DecideInput input_;
  mutable std::optional<std::optional<Candidate>> memo_;
};

struct TurnFlow::Move
{
  const TurnRequest& request;
  Candidate candidate;
  std::string slot;
  int attempts{0};
  bool answering{false};
  std::string utterance{};
  std::optional<int64_t> heardAt{};
};

TurnFlow::TurnFlow(FlowDeps deps)
    : executor_(deps.executor), decider_(deps.decider), text_(deps.text), policies_(std::move(deps.policies))
{
}

void TurnFlow::clearAll(int64_t userId)
{
  pendings_.forget(userId);
  executor_.forgetPending(userId);
}

Outcome TurnFlow::declined() const
{
  Outcome outcome;
  outcome.source = "declined";
  outcome.findings.push_back({.kind = FindingKind::Declined, .tool = {}, .text = {}});
  return outcome;
}

Outcome TurnFlow::unactionable(const TurnRequest& request) const
{
  Outcome outcome;
  if (reply_claims::asksForAction(request.utterance))
    outcome.findings.push_back({.kind = FindingKind::Unactionable, .tool = {}, .text = {}});
  return outcome;
}

void TurnFlow::execute(const TurnRequest& request, tools::ToolCall call, Outcome& outcome)
{
  const std::string spoken = call.context.utterance.empty() ? std::string(request.utterance) : call.context.utterance;
  const std::optional<int64_t> heard = call.context.heardAt;
  call.context = request.context;
  call.context.utterance = spoken;
  call.context.heardAt = heard;
  call.context.decided = true;
  const auto started = std::chrono::steady_clock::now();
  tools::ToolResult result = executor_.execute(call, request.audience);
  outcome.toolMs += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  if (result.ok)
    LOG_INFO << "TurnFlow: '" << call.name << "' ok: " << result.output;
  else
    LOG_WARN << "TurnFlow: '" << call.name << "' not done (" << result.code << "): " << result.output;
  const tools::ToolDescriptor* tool = handleOf(request.offered, call.name);
  outcome.wrote = outcome.wrote || (tool != nullptr && changes(result, *tool));
  outcome.opened = outcome.opened || (tool != nullptr && shows(result, *tool));
  outcome.called = outcome.called || (result.ok && result.data["callScheduled"].isBool() && result.data["callScheduled"].asBool());
  outcome.findings.push_back(findingOf(result));
  outcome.steps.push_back({.call = std::move(call), .result = std::move(result)});
}

Outcome TurnFlow::ask(const Move& move)
{
  Outcome outcome;
  outcome.source = move.candidate.source;
  const std::string_view lang = move.request.context.lang;
  if (move.attempts > kMaxAttempts) {
    outcome.question = turn_texts::misunderstood(lang);
    return outcome;
  }
  if (move.request.context.userId > 0)
    pendings_.put(move.request.context.userId,
                  {.awaiting = Awaiting::Slot,
                   .candidate = move.candidate,
                   .alternative = std::nullopt,
                   .slot = move.slot,
                   .utterance = {},
                   .attempts = move.attempts,
                   .at = {}});
  outcome.question = turn_texts::slotQuestion({.tool = move.candidate.tool, .slot = move.slot, .lang = lang});
  return outcome;
}

Outcome TurnFlow::proceed(const Move& move)
{
  const TurnRequest& request = move.request;
  Outcome outcome;
  outcome.source = move.candidate.source;
  const tools::ToolDescriptor* tool = handleOf(request.offered, move.candidate.tool);
  if (tool == nullptr)
    return outcome;

  tools::ToolCall call{.name = move.candidate.tool, .arguments = move.candidate.arguments, .context = request.context};
  call.context.utterance = move.utterance.empty() ? std::string(request.utterance) : move.utterance;
  call.context.heardAt = move.heardAt;
  if (!move.answering && !move.heardAt && move.candidate.tool == kRemindTool) {
    const CallReading reading = slots::dayReading({.utterance = call.context.utterance, .lang = request.context.lang, .now = request.now});
    if (reading.conflict)
      return askDay(move, move.candidate, {.field = std::string(kHeardField), .conflict = *reading.conflict});
    if (reading.farAway)
      return askFar(move, move.candidate, std::string(kHeardField));
  }
  const std::vector<std::string> fields =
      move.answering ? std::vector<std::string>{move.slot} : missingFields(tool->spec, move.candidate, move.candidate.arguments);
  const slots::Filled filled = slots::fill({.spec = tool->spec,
                                            .fields = fields,
                                            .arguments = move.candidate.arguments,
                                            .context = call.context,
                                            .now = request.now,
                                            .text = *text_,
                                            .modules = request.audience.modules,
                                            .answering = move.answering});
  Candidate next = move.candidate;
  next.arguments = filled.arguments;
  if (filled.dispute)
    return askDay(move, next, *filled.dispute);
  if (filled.farField)
    return askFar(move, next, *filled.farField);
  if (!filled.missing.empty())
    return ask({.request = request, .candidate = std::move(next), .slot = filled.missing.front(), .attempts = move.attempts + 1, .answering = false});
  if (move.answering)
    if (const auto remaining = missingFields(tool->spec, next, next.arguments); !remaining.empty())
      return ask({.request = request, .candidate = std::move(next), .slot = remaining.front(), .attempts = 0, .answering = false});

  call.arguments = std::move(next.arguments);
  execute(request, std::move(call), outcome);
  return projectRefusal(request, move.candidate, std::move(outcome));
}

Outcome TurnFlow::projectRefusal(const TurnRequest& request, const Candidate& candidate, Outcome outcome)
{
  if (outcome.steps.empty())
    return outcome;
  const tools::ToolResult& result = outcome.steps.back().result;
  const bool listed = result.code == kProjectNeeded || result.code == kProjectAmbiguous || result.code == kProjectUnknown;
  if (!listed && result.code != kNoProjects)
    return outcome;
  Candidate held = candidate;
  held.arguments = outcome.steps.back().call.arguments;
  held.arguments.removeMember("project");
  held.arguments.removeMember("project_id");
  std::vector<std::string> options;
  for (const Json::Value& name : result.data["projects"])
    if (name.isString())
      options.push_back(name.asString());
  if (listed && options.empty())
    return outcome;
  outcome.findings.pop_back();
  const std::string_view lang = request.context.lang;
  if (request.context.userId > 0)
    pendings_.put(request.context.userId,
                  {.awaiting = listed ? Awaiting::Project : Awaiting::NewProject,
                   .candidate = held,
                   .alternative = std::nullopt,
                   .slot = "project",
                   .utterance = std::string(request.utterance),
                   .attempts = 1,
                   .at = {},
                   .options = options,
                   .held = std::nullopt});
  outcome.question = listed ? turn_texts::projectQuestion({.options = options, .lang = lang}) : turn_texts::newProjectQuestion(lang, true);
  return outcome;
}

Outcome TurnFlow::askDay(const Move& move, const Candidate& candidate, const slots::Dispute& dispute)
{
  const TurnRequest& request = move.request;
  Outcome outcome;
  outcome.source = candidate.source;
  const std::string_view lang = request.context.lang;
  const int attempts = move.attempts + 1;
  if (attempts > kMaxAttempts) {
    outcome.question = turn_texts::misunderstood(lang);
    return outcome;
  }
  if (request.context.userId > 0)
    pendings_.put(request.context.userId,
                  {.awaiting = Awaiting::Day,
                   .candidate = candidate,
                   .alternative = std::nullopt,
                   .slot = dispute.field,
                   .utterance = move.utterance.empty() ? std::string(request.utterance) : move.utterance,
                   .attempts = attempts,
                   .at = {},
                   .options = {},
                   .held = std::nullopt,
                   .values = {iso_time::format(dispute.conflict.byWeekday), iso_time::format(dispute.conflict.byDate)},
                   .relative = dispute.conflict.relative});
  outcome.question = turn_texts::dayQuestion({.byWeekday = dispute.conflict.byWeekday,
                                              .byDate = dispute.conflict.byDate,
                                              .relative = dispute.conflict.relative,
                                              .now = request.now,
                                              .lang = lang});
  return outcome;
}

Outcome TurnFlow::askFar(const Move& move, const Candidate& candidate, const std::string& field)
{
  const TurnRequest& request = move.request;
  Outcome outcome;
  outcome.source = candidate.source;
  const int attempts = move.attempts + 1;
  if (attempts > kMaxAttempts) {
    outcome.question = turn_texts::misunderstood(request.context.lang);
    return outcome;
  }
  if (request.context.userId > 0)
    pendings_.put(request.context.userId,
                  {.awaiting = Awaiting::Day,
                   .candidate = candidate,
                   .alternative = std::nullopt,
                   .slot = field,
                   .utterance = move.utterance.empty() ? std::string(request.utterance) : move.utterance,
                   .attempts = attempts,
                   .at = {},
                   .options = {},
                   .held = std::nullopt,
                   .values = {},
                   .relative = -1});
  outcome.question = turn_texts::farQuestion(request.context.lang);
  return outcome;
}

std::optional<Outcome> TurnFlow::followUpDay(const TurnRequest& request, const Deciding& deciding, const Pending& pending)
{
  const int64_t userId = request.context.userId;
  pendings_.forget(userId);
  if (supersedes(deciding)) {
    clearAll(userId);
    return std::nullopt;
  }
  if (declining(request.utterance)) {
    clearAll(userId);
    return declined();
  }
  const Move again{.request = request,
                   .candidate = pending.candidate,
                   .slot = pending.slot,
                   .attempts = pending.attempts,
                   .answering = false,
                   .utterance = pending.utterance};
  std::optional<std::string> chosen;
  if (pending.values.empty()) {
    const CallReading reading = slots::dayReading({.utterance = request.utterance, .lang = request.context.lang, .now = request.now});
    if (reading.conflict)
      return askDay(again, pending.candidate, {.field = pending.slot, .conflict = *reading.conflict});
    if (!reading.time)
      return askFar(again, pending.candidate, pending.slot);
    chosen = iso_time::format(reading.time->fireAt);
  }
  else {
    const auto picked = slots::chooseDay({.utterance = request.utterance, .values = pending.values, .relative = pending.relative});
    if (!picked) {
      const auto at = [&pending](std::size_t index) { return iso_time::parse(pending.values.at(index)).value_or(0); };
      return askDay(again, pending.candidate, {.field = pending.slot, .conflict = {.byWeekday = at(0), .byDate = at(1), .phraseBegin = 0, .phraseEnd = 0, .relative = pending.relative}});
    }
    chosen = pending.values.at(*picked);
  }
  Candidate next = pending.candidate;
  std::optional<int64_t> heard;
  if (pending.slot == kHeardField)
    heard = iso_time::parse(*chosen);
  else
    next.arguments[pending.slot] = *chosen;
  return proceed({.request = request,
                  .candidate = std::move(next),
                  .slot = {},
                  .attempts = 0,
                  .answering = false,
                  .utterance = pending.utterance,
                  .heardAt = heard});
}

Outcome TurnFlow::askProjectAgain(const TurnRequest& request, const Pending& pending)
{
  Outcome outcome;
  outcome.source = pending.candidate.source;
  const std::string_view lang = request.context.lang;
  if (pending.attempts + 1 > kMaxAttempts) {
    outcome.question = turn_texts::misunderstood(lang);
    return outcome;
  }
  Pending next = pending;
  next.attempts = pending.attempts + 1;
  pendings_.put(request.context.userId, next);
  outcome.question = pending.awaiting == Awaiting::Project ? turn_texts::projectQuestion({.options = pending.options, .lang = lang})
                                                           : turn_texts::newProjectQuestion(lang, false);
  return outcome;
}

Outcome TurnFlow::offerProject(const TurnRequest& request, const Pending& pending, const std::string& name)
{
  Outcome outcome;
  outcome.source = pending.candidate.source;
  const std::string_view lang = request.context.lang;
  if (handleOf(request.offered, kCreateProjectTool) == nullptr) {
    outcome.findings.push_back({.kind = FindingKind::Refused, .tool = std::string(kCreateProjectTool), .text = turn_texts::cannotCreateProject(lang)});
    return outcome;
  }
  Candidate create{.tool = std::string(kCreateProjectTool),
                   .arguments = Json::Value(Json::objectValue),
                   .fill = {},
                   .confidence = 1.0,
                   .source = "project offer",
                   .decider = pending.candidate.decider,
                   .exact = true,
                   .confident = true,
                   .runnerUp = std::nullopt,
                   .now = std::nullopt};
  create.arguments["name"] = name;
  pendings_.put(request.context.userId,
                {.awaiting = Awaiting::Approval,
                 .candidate = std::move(create),
                 .alternative = std::nullopt,
                 .slot = {},
                 .utterance = pending.utterance,
                 .attempts = 0,
                 .at = {},
                 .options = {},
                 .held = pending.candidate});
  outcome.question = turn_texts::createProjectQuestion({.name = name, .lang = lang});
  return outcome;
}

Outcome TurnFlow::confirmed(const TurnRequest& request, const Pending& pending)
{
  Outcome first = proceed({.request = request,
                           .candidate = pending.candidate,
                           .slot = {},
                           .attempts = 0,
                           .answering = false,
                           .utterance = pending.utterance});
  if (!pending.held || !first.wrote)
    return first;
  Candidate task = *pending.held;
  task.arguments["project"] = pending.candidate.arguments["name"];
  Outcome second = proceed({.request = request, .candidate = std::move(task), .slot = {}, .attempts = 0, .answering = false, .utterance = pending.utterance});
  first.toolMs += second.toolMs;
  first.wrote = first.wrote || second.wrote;
  first.question = std::move(second.question);
  for (Step& step : second.steps)
    first.steps.push_back(std::move(step));
  for (Finding& finding : second.findings)
    first.findings.push_back(std::move(finding));
  return first;
}

std::optional<Outcome> TurnFlow::followUpProject(const TurnRequest& request, const Deciding& deciding, const Pending& pending)
{
  const int64_t userId = request.context.userId;
  pendings_.forget(userId);
  if (supersedes(deciding)) {
    clearAll(userId);
    return std::nullopt;
  }
  if (declining(request.utterance)) {
    clearAll(userId);
    return declined();
  }
  if (pending.awaiting == Awaiting::NewProject) {
    const auto name = slots::nameGiven(request.utterance);
    const auto given = name ? name : slots::answerText(request.utterance);
    if (!given)
      return askProjectAgain(request, pending);
    return offerProject(request, pending, *given);
  }
  if (slots::namesNewOne(request.utterance)) {
    if (const auto name = slots::nameGiven(request.utterance))
      return offerProject(request, pending, *name);
    Pending next = pending;
    next.awaiting = Awaiting::NewProject;
    next.attempts = 1;
    pendings_.put(userId, next);
    Outcome outcome;
    outcome.source = pending.candidate.source;
    outcome.question = turn_texts::newProjectQuestion(request.context.lang, false);
    return outcome;
  }
  const slots::Choice choice = slots::choose(request.utterance, pending.options);
  if (choice.kind != slots::ChoiceKind::Chosen)
    return askProjectAgain(request, pending);
  Candidate task = pending.candidate;
  task.arguments["project"] = choice.name;
  return proceed({.request = request, .candidate = std::move(task), .slot = {}, .attempts = 0, .answering = false, .utterance = pending.utterance});
}

Outcome TurnFlow::confirm(const TurnRequest& request, const Candidate& candidate)
{
  Outcome outcome;
  outcome.source = candidate.source;
  Candidate held = candidate;
  const tools::ToolDescriptor* tool = handleOf(request.offered, candidate.tool);
  if (tool != nullptr) {
    tools::ToolContext context = request.context;
    context.utterance = std::string(request.utterance);
    const std::vector<std::string> fields = missingFields(tool->spec, candidate, candidate.arguments);
    const slots::Filled filled = slots::fill({.spec = tool->spec,
                                              .fields = fields,
                                              .arguments = candidate.arguments,
                                              .context = context,
                                              .now = request.now,
                                              .text = *text_,
                                              .modules = request.audience.modules,
                                              .answering = false});
    held.arguments = filled.arguments;
  }
  if (request.context.userId > 0)
    pendings_.put(request.context.userId,
                  {.awaiting = Awaiting::Approval,
                   .candidate = held,
                   .alternative = std::nullopt,
                   .slot = {},
                   .utterance = std::string(request.utterance),
                   .attempts = 0,
                   .at = {}});
  outcome.question = turn_texts::confirmQuestion(
      {.tool = held.tool, .lang = request.context.lang, .details = detailsOf(held, request)});
  return outcome;
}

Outcome TurnFlow::choose(const TurnRequest& request, const Candidate& candidate)
{
  if (!candidate.runnerUp || handleOf(request.offered, candidate.runnerUp->tool) == nullptr)
    return confirm(request, candidate);
  Outcome outcome;
  outcome.source = candidate.source;
  if (request.context.userId > 0)
    pendings_.put(request.context.userId,
                  {.awaiting = Awaiting::Choice,
                   .candidate = candidate,
                   .alternative = candidateOf(*candidate.runnerUp, candidate),
                   .slot = {},
                   .utterance = std::string(request.utterance),
                   .attempts = 0,
                   .at = {}});
  outcome.question = turn_texts::chooseQuestion({.first = candidate.tool, .second = candidate.runnerUp->tool, .lang = request.context.lang});
  return outcome;
}

bool TurnFlow::corroborated(const SecondOpinion& opinion) const
{
  if (secondSignal_)
    return secondSignal_(opinion);
  if (const double nowMin = policies_.of(opinion.candidate.decider).nowMin; nowMin > 0.0)
    return opinion.candidate.now && *opinion.candidate.now >= nowMin;
  for (const Decider* witness : witnesses_) {
    if (witness->id() == opinion.candidate.decider)
      continue;
    const auto other = witness->decide(opinion.input);
    if (other && other->confident && other->tool == opinion.candidate.tool && levelOf(*other) == Verdict::Act)
      return true;
  }
  return false;
}

bool TurnFlow::supersedes(const Deciding& deciding) const
{
  const auto& fresh = deciding.get();
  return fresh && verdictOf(*fresh) == Verdict::Act;
}

Verdict TurnFlow::levelOf(const Candidate& candidate) const
{
  Reading reading{.confidence = candidate.confidence, .runnerUp = std::nullopt};
  if (candidate.runnerUp)
    reading.runnerUp = candidate.runnerUp->confidence;
  return judge(policies_.of(candidate.decider), reading);
}

Verdict TurnFlow::verdictOf(const Candidate& candidate) const
{
  const Verdict verdict = levelOf(candidate);
  return verdict == Verdict::Act && !candidate.confident ? Verdict::Ask : verdict;
}

bool TurnFlow::needsSecondSignal(const Candidate& candidate) const
{
  return !isReadOnlyTool(candidate.tool) && (!candidate.exact || policies_.of(candidate.decider).witnessOnly);
}

std::optional<Outcome> TurnFlow::followUpPreview(const TurnRequest& request, const Deciding& deciding, const PendingPreview& preview)
{
  const int64_t userId = request.context.userId;
  if (affirmed(request.utterance) && handleOf(request.offered, preview.tool) != nullptr) {
    Outcome outcome;
    outcome.source = "confirmation";
    execute(request, {.name = preview.tool, .arguments = preview.arguments, .context = {}}, outcome);
    clearAll(userId);
    return outcome;
  }
  clearAll(userId);
  if (supersedes(deciding))
    return std::nullopt;
  if (declining(request.utterance))
    return declined();
  return std::nullopt;
}

std::optional<Outcome> TurnFlow::followUpOffer(const TurnRequest& request, const Deciding& deciding, const PendingOffer& offer)
{
  const int64_t userId = request.context.userId;
  const std::string_view tool = request.audience.role == UserRole::Owner ? kEnableTool : kRequestTool;
  if (affirmed(request.utterance) && handleOf(request.offered, tool) != nullptr) {
    Json::Value arguments(Json::objectValue);
    arguments["module"] = offer.module;
    Outcome outcome;
    outcome.source = "offer accepted";
    execute(request, {.name = std::string(tool), .arguments = std::move(arguments), .context = {}}, outcome);
    clearAll(userId);
    return outcome;
  }
  clearAll(userId);
  if (supersedes(deciding))
    return std::nullopt;
  if (declining(request.utterance))
    return declined();
  return std::nullopt;
}

std::optional<Outcome> TurnFlow::followUpOwn(const TurnRequest& request, const Deciding& deciding, const Pending& pending)
{
  const int64_t userId = request.context.userId;
  if (pending.awaiting == Awaiting::Project || pending.awaiting == Awaiting::NewProject)
    return followUpProject(request, deciding, pending);
  if (pending.awaiting == Awaiting::Day)
    return followUpDay(request, deciding, pending);
  if (pending.awaiting == Awaiting::Choice) {
    pendings_.forget(userId);
    if (pending.alternative && slots::namesOther(request.utterance))
      return proceed({.request = request,
                      .candidate = *pending.alternative,
                      .slot = {},
                      .attempts = 0,
                      .answering = false,
                      .utterance = pending.utterance});
    if (affirmed(request.utterance))
      return proceed({.request = request,
                      .candidate = pending.candidate,
                      .slot = {},
                      .attempts = 0,
                      .answering = false,
                      .utterance = pending.utterance});
    clearAll(userId);
    if (supersedes(deciding))
      return std::nullopt;
    if (declining(request.utterance))
      return declined();
    return std::nullopt;
  }
  if (pending.awaiting == Awaiting::Approval) {
    pendings_.forget(userId);
    if (affirmed(request.utterance))
      return confirmed(request, pending);
    clearAll(userId);
    if (supersedes(deciding))
      return std::nullopt;
    if (declining(request.utterance))
      return declined();
    return std::nullopt;
  }
  if (supersedes(deciding)) {
    clearAll(userId);
    return std::nullopt;
  }
  pendings_.forget(userId);
  if (declining(request.utterance)) {
    clearAll(userId);
    return declined();
  }
  return proceed({.request = request, .candidate = pending.candidate, .slot = pending.slot, .attempts = pending.attempts, .answering = true});
}

std::optional<Outcome> TurnFlow::followUp(const TurnRequest& request, const Deciding& deciding)
{
  const int64_t userId = request.context.userId;
  if (userId <= 0)
    return std::nullopt;
  const auto own = pendings_.peek(userId);
  const auto preview = executor_.pendingPreview(userId);
  const auto offer = executor_.pendingOffer(userId);
  const std::chrono::steady_clock::time_point never{};
  const auto stamp = [&never](const auto& pending) { return pending ? pending->at : never; };
  const auto newest = std::max({stamp(own), stamp(preview), stamp(offer)});
  if (own && own->at == newest)
    return followUpOwn(request, deciding, *own);
  if (preview && preview->at == newest)
    return followUpPreview(request, deciding, *preview);
  if (offer && offer->at == newest)
    return followUpOffer(request, deciding, *offer);
  return std::nullopt;
}

Outcome TurnFlow::decided(const TurnRequest& request, const Deciding& deciding)
{
  const auto& candidate = deciding.get();
  if (!candidate)
    return unactionable(request);
  const tools::ToolDescriptor* tool = handleOf(request.offered, candidate->tool);
  if (tool == nullptr)
    return unactionable(request);
  Verdict verdict = verdictOf(*candidate);
  std::string_view named = verdictName(verdict);
  if (verdict == Verdict::Act && needsSecondSignal(*candidate) &&
      !corroborated({.candidate = *candidate, .input = deciding.input()})) {
    verdict = Verdict::Ask;
    named = "guard";
  }
  tally_.note({.decider = candidate->decider,
               .exact = candidate->exact,
               .tool = candidate->tool,
               .lang = request.context.lang,
               .verdict = named});
  switch (verdict) {
    case Verdict::Act:
      return proceed({.request = request, .candidate = *candidate, .slot = {}, .attempts = 0, .answering = false});
    case Verdict::Ask:
      return confirm(request, *candidate);
    case Verdict::Choose:
      return choose(request, *candidate);
    case Verdict::Pass:
      break;
  }
  return unactionable(request);
}

Outcome TurnFlow::run(const TurnRequest& request)
{
  if (request.utterance.empty())
    return {};
  const Deciding deciding(*decider_, {.utterance = request.utterance,
                                      .lang = request.context.lang,
                                      .offered = request.offered,
                                      .modules = request.audience.modules,
                                      .previousAssistant = request.previousAssistant});
  if (auto followed = followUp(request, deciding))
    return *std::move(followed);
  return decided(request, deciding);
}

std::string TurnFlow::notes(const Outcome& outcome, std::string_view lang)
{
  std::string out;
  for (const Finding& finding : outcome.findings) {
    if (!out.empty())
      out += '\n';
    out += noteOf(finding, lang);
  }
  return out;
}

}
