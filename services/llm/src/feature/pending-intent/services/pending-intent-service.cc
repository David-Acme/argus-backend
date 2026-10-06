#include "pending-intent-service.hxx"

#include <json/reader.h>
#include <json/writer.h>
#include <text/iso-time.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <set>
#include <string_view>
#include <utility>

namespace
{
constexpr std::array<const char*, 3> kTimeArguments{"starts_at", "due_at", "target_at"};
constexpr std::array<const char*, 4> kTitleArguments{"title", "name", "text", "query"};
constexpr size_t kSummaryLimit = 160;
constexpr int64_t kSoonS = 60;

struct Reason
{
  std::string_view code;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<Reason, 8> kReasons{{{.code = "disk_full", .es = "no hay espacio en el disco", .en = "the disk is full"},
                                          {.code = "network", .es = "falló la descarga", .en = "the download failed"},
                                          {.code = "source_unavailable", .es = "la fuente de descarga no responde", .en = "the download source does not answer"},
                                          {.code = "checksum_mismatch", .es = "los archivos descargados no son los esperados", .en = "the downloaded files are not the expected ones"},
                                          {.code = "hardware_insufficient", .es = "este equipo no alcanza", .en = "this machine cannot run it"},
                                          {.code = "health_check_failed", .es = "no arrancó bien", .en = "it did not start well"},
                                          {.code = "timeout", .es = "tardó demasiado", .en = "it took too long"},
                                          {.code = "cancelled", .es = "se canceló la instalación", .en = "the installation was cancelled"}}};

Json::Value parsed(const std::string& text)
{
  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string errors;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors) || !value.isObject())
    value = Json::Value(Json::objectValue);
  return value;
}

std::string serialized(const Json::Value& value)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

std::string clipped(std::string text)
{
  if (text.size() > kSummaryLimit)
    text.resize(kSummaryLimit);
  return text;
}

std::string summaryOf(const PendingIntentRow& row, const Json::Value& arguments)
{
  for (const char* name : kTitleArguments)
    if (arguments[name].isString() && !arguments[name].asString().empty())
      return clipped(arguments[name].asString());
  return clipped(row.utterance.empty() ? row.tool : row.utterance);
}

int64_t reminderTime(const Json::Value& arguments, int64_t now)
{
  for (const char* name : kTimeArguments) {
    if (!arguments[name].isString())
      continue;
    if (const auto at = iso_time::parse(arguments[name].asString()); at && *at > now)
      return *at;
  }
  return now + kSoonS;
}

std::string reasonText(const std::string& reason, bool english)
{
  const auto found = std::ranges::find(kReasons, std::string_view(reason), &Reason::code);
  if (found != kReasons.end())
    return std::string(english ? found->en : found->es);
  return reason.empty() ? std::string(english ? "it could not be completed" : "no se pudo completar") : reason;
}
}

PendingIntentService::PendingIntentService(PendingIntentDependencies dependencies, PendingIntentLimits limits)
    : dependencies_(std::move(dependencies)), limits_(limits)
{
}

PendingIntentService::~PendingIntentService()
{
  requestStop();
}

int64_t PendingIntentService::now() const
{
  return dependencies_.clock ? dependencies_.clock() : static_cast<int64_t>(std::time(nullptr));
}

void PendingIntentService::offered(const tools::IntentOffer& offer)
{
  if (dependencies_.graph == nullptr || offer.userId <= 0)
    return;
  const std::scoped_lock lock(dependencies_.graph->mutex());
  static_cast<void>(repository_.offer(dependencies_.graph->handle(),
                                      {.userId = offer.userId,
                                       .role = userRoleToString(offer.role),
                                       .module = offer.module,
                                       .tool = offer.tool,
                                       .arguments = serialized(offer.arguments),
                                       .lang = offer.lang,
                                       .utterance = offer.utterance,
                                       .sessionId = offer.sessionId,
                                       .at = now()}));
}

void PendingIntentService::accepted(const tools::ModuleAcceptance& acceptance)
{
  if (dependencies_.graph == nullptr)
    return;
  bool any = false;
  {
    const std::scoped_lock lock(dependencies_.graph->mutex());
    any = !repository_.accept(dependencies_.graph->handle(),
                              {.userId = acceptance.userId, .module = acceptance.module, .at = now()})
               .empty();
  }
  if (any)
    postActivated(acceptance.module);
}

std::vector<PendingIntentRow> PendingIntentService::waitingFor(const std::string& module)
{
  if (dependencies_.graph == nullptr)
    return {};
  const std::scoped_lock lock(dependencies_.graph->mutex());
  return repository_.waiting(dependencies_.graph->handle(), module);
}

bool PendingIntentService::settle(const PendingIntentSettleInput& input)
{
  const std::scoped_lock lock(dependencies_.graph->mutex());
  return repository_.settle(dependencies_.graph->handle(), input);
}

void PendingIntentService::tell(const PendingIntentRow& row, const IntentNotice& notice) const
{
  if (!dependencies_.notifier)
    return;
  if (!dependencies_.notifier->tell(notice))
    LOG_WARN << "argus-llm: the user " << row.userId << " was not told about their request " << row.id;
}

void PendingIntentService::execute(const PendingIntentRow& row)
{
  const Json::Value arguments = parsed(row.arguments);
  tools::ToolCall call;
  call.name = row.tool;
  call.arguments = arguments;
  call.context.userId = row.userId;
  call.context.role = userRoleFromString(row.role);
  call.context.lang = row.lang;
  call.context.sessionId = row.sessionId;
  call.context.utterance = row.utterance;
  call.context.decided = true;
  if (!dependencies_.run)
    return;
  const tools::ToolResult result = dependencies_.run(call, call.context.role);
  if (result.code == "module_inactive")
    return;
  if (!result.ok) {
    fail(row, result.output);
    return;
  }
  if (!settle({.id = row.id, .state = PendingIntentState::Done, .detail = "", .at = now()}))
    return;
  const bool english = row.lang == "en";
  const std::string name = dependencies_.moduleName ? dependencies_.moduleName({.module = row.module, .lang = row.lang}) : row.module;
  tell(row,
       {.userId = row.userId,
        .title = english ? "Your request is done" : "Tu petición está lista",
        .body = (english ? "I turned on " + name + " and completed it. " : "Activé " + name + " y la completé. ") + result.output,
        .commandId = "intent-" + std::to_string(row.id) + "-done"});
}

void PendingIntentService::fail(const PendingIntentRow& row, const std::string& reason)
{
  if (!settle({.id = row.id, .state = PendingIntentState::Failed, .detail = reason, .at = now()}))
    return;
  const Json::Value arguments = parsed(row.arguments);
  const std::string summary = summaryOf(row, arguments);
  const bool english = row.lang == "en";
  const bool saved = dependencies_.reminders &&
                     dependencies_.reminders->create({.userId = row.userId,
                                                      .role = row.role,
                                                      .title = summary,
                                                      .scheduledAt = reminderTime(arguments, now()),
                                                      .commandId = "intent-" + std::to_string(row.id)});
  const std::string name = dependencies_.moduleName ? dependencies_.moduleName({.module = row.module, .lang = row.lang}) : row.module;
  const std::string why = reasonText(reason, english);
  std::string body = english ? "I could not turn on " + name + " (" + why + "). " : "No pude activar " + name + " (" + why + "). ";
  if (saved)
    body += english ? "I left your request as a reminder: «" + summary + "»." : "Dejé tu petición como recordatorio: «" + summary + "».";
  else
    body += english ? "I could not save it as a reminder either." : "Tampoco pude guardarla como recordatorio.";
  tell(row,
       {.userId = row.userId,
        .title = english ? "I could not complete your request" : "No pude completar tu petición",
        .body = std::move(body),
        .commandId = "intent-" + std::to_string(row.id) + "-failed"});
}

void PendingIntentService::moduleActivated(const std::string& module)
{
  for (const auto& row : waitingFor(module))
    execute(row);
}

void PendingIntentService::moduleFailed(const ModuleFailure& failure)
{
  for (const auto& row : waitingFor(failure.module))
    fail(row, failure.reason);
}

void PendingIntentService::sweep()
{
  if (dependencies_.graph == nullptr)
    return;
  const int64_t at = now();
  std::vector<PendingIntentRow> stale;
  {
    const std::scoped_lock lock(dependencies_.graph->mutex());
    static_cast<void>(repository_.expireOffers(dependencies_.graph->handle(), at - limits_.offerTtlS, at));
    stale = repository_.waitingBefore(dependencies_.graph->handle(), at - limits_.waitTtlS);
    static_cast<void>(repository_.purge(dependencies_.graph->handle(), at - limits_.keepSettledS));
  }
  for (const auto& row : stale)
    fail(row, "timeout");
  std::set<std::string> modules;
  for (const auto& row : waitingFor({}))
    if (dependencies_.moduleActive && dependencies_.moduleActive(row.module))
      modules.insert(row.module);
  for (const auto& module : modules)
    moduleActivated(module);
}

void PendingIntentService::post(Event event)
{
  {
    const std::scoped_lock lock(mutex_);
    events_.push_back(std::move(event));
  }
  wake_.notify_all();
}

void PendingIntentService::postActivated(std::string module)
{
  post({.kind = Event::Kind::Activated, .module = std::move(module), .reason = {}});
}

void PendingIntentService::postFailed(ModuleFailure failure)
{
  post({.kind = Event::Kind::Failed, .module = std::move(failure.module), .reason = std::move(failure.reason)});
}

void PendingIntentService::postSweep()
{
  post({.kind = Event::Kind::Sweep, .module = {}, .reason = {}});
}

void PendingIntentService::handle(const Event& event)
{
  try {
    switch (event.kind) {
      case Event::Kind::Activated:
        moduleActivated(event.module);
        break;
      case Event::Kind::Failed:
        moduleFailed({.module = event.module, .reason = event.reason});
        break;
      case Event::Kind::Sweep:
        sweep();
        break;
    }
  }
  catch (const std::exception& error) {
    LOG_WARN << "argus-llm: pending intents: " << error.what();
  }
}

void PendingIntentService::start()
{
  if (worker_.joinable())
    return;
  running_.store(true, std::memory_order_release);
  worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
  postSweep();
}

void PendingIntentService::run(const std::stop_token& stop)
{
  while (!stop.stop_requested()) {
    Event event;
    bool have = false;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, stop, limits_.sweepEvery, [this] { return !events_.empty(); });
      if (!events_.empty()) {
        event = std::move(events_.front());
        events_.pop_front();
        have = true;
      }
    }
    if (stop.stop_requested())
      break;
    handle(have ? event : Event{.kind = Event::Kind::Sweep, .module = {}, .reason = {}});
  }
}

void PendingIntentService::requestStop()
{
  worker_.request_stop();
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
  running_.store(false, std::memory_order_release);
}

bool PendingIntentService::drained() const
{
  return !running_.load(std::memory_order_acquire);
}
