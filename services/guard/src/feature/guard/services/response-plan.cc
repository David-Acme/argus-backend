#include "response-plan.hxx"

#include <algorithm>
#include <ranges>

namespace
{
int roleOrder(UserRole role)
{
  switch (role) {
    case UserRole::Owner:
      return 0;
    case UserRole::Guard:
      return 1;
    case UserRole::Resident:
      return 2;
    case UserRole::Guest:
      return 3;
  }
  return 3;
}

RecipientMode defaultMode(UserRole role)
{
  return role == UserRole::Guest ? RecipientMode::Off : RecipientMode::Call;
}

const PresenceRow* presenceOf(std::span<const PresenceRow> rows, int64_t userId)
{
  const auto row = std::ranges::find(rows, userId, &PresenceRow::userId);
  return row == rows.end() ? nullptr : &*row;
}

bool isHome(std::span<const PresenceRow> rows, int64_t userId)
{
  const PresenceRow* row = presenceOf(rows, userId);
  return row != nullptr && row->state == PresenceState::Home;
}

void compactSteps(std::vector<ResponsePlanEntry>& entries)
{
  std::vector<int> steps;
  steps.reserve(entries.size());
  for (const auto& entry : entries)
    steps.push_back(entry.step);
  std::ranges::sort(steps);
  const auto [first, last] = std::ranges::unique(steps);
  steps.erase(first, last);
  for (auto& entry : entries)
    entry.step = static_cast<int>(std::ranges::lower_bound(steps, entry.step) -
                                  steps.begin());
}

struct PresenceSummary
{
  bool anyHome{false};
  bool allHome{false};
  bool confirmedAway{false};
};

PresenceSummary summarize(std::span<const ResponsePlanEntry> entries,
                          std::span<const PresenceRow> presence)
{
  PresenceSummary summary;
  bool anyAway = false;
  bool everyoneAway = !entries.empty();
  bool tunnel = false;
  for (const auto& entry : entries) {
    const PresenceRow* row = presenceOf(presence, entry.userId);
    const PresenceState state = row ? row->state : PresenceState::Unknown;
    if (state == PresenceState::Home)
      summary.anyHome = true;
    if (state == PresenceState::Away) {
      anyAway = true;
      tunnel = tunnel || row->source == PresenceSource::TunnelSession;
    }
    else {
      everyoneAway = false;
    }
  }
  summary.allHome = summary.anyHome && !anyAway;
  summary.confirmedAway = everyoneAway && tunnel;
  return summary;
}

ResponseStrategy strategyFor(const ResponsePlanInput& input,
                             const PresenceSummary& summary)
{
  switch (input.trigger) {
    case ResponseTrigger::Panic:
    case ResponseTrigger::Duress:
    case ResponseTrigger::Escalation:
      return ResponseStrategy::Everyone;
    case ResponseTrigger::Tamper:
      return ResponseStrategy::Ordered;
    case ResponseTrigger::Intrusion:
      break;
  }
  if (summary.anyHome && input.indoor)
    return ResponseStrategy::Everyone;
  if (input.night && summary.allHome && !input.clearThreat)
    return ResponseStrategy::NightQuiet;
  if (summary.anyHome)
    return ResponseStrategy::InsideFirst;
  return ResponseStrategy::Ordered;
}
}

std::vector<ResponseMember>
response_plan::members(const ResponseMembersInput& input)
{
  int maxExplicit = 0;
  for (const auto& row : input.rows) {
    if (row.step)
      maxExplicit = std::max(maxExplicit, *row.step);
  }
  int nextResident = maxExplicit + 1;
  std::vector<ResponseMember> members;
  members.reserve(input.users.size());
  for (const auto& user : input.users) {
    if (!user.active || user.userId <= 0)
      continue;
    const auto row = std::ranges::find(input.rows, user.userId,
                                       &ResponseRecipientRow::userId);
    const bool hasRow = row != input.rows.end();
    ResponseMember member{.userId = user.userId,
                          .role = user.role,
                          .name = user.name,
                          .mode = defaultMode(user.role),
                          .step = 0,
                          .onDuty = hasRow && row->onDuty,
                          .customized = hasRow && (row->mode || row->step)};
    if (hasRow && row->mode)
      member.mode = *row->mode;
    if (hasRow && row->step)
      member.step = std::clamp(*row->step, 0, kMaxStep);
    else if (user.role == UserRole::Resident)
      member.step = std::min(nextResident++, kMaxStep);
    members.push_back(std::move(member));
  }
  std::ranges::sort(members, [](const ResponseMember& left,
                                const ResponseMember& right) {
    if (left.step != right.step)
      return left.step < right.step;
    if (roleOrder(left.role) != roleOrder(right.role))
      return roleOrder(left.role) < roleOrder(right.role);
    return left.userId < right.userId;
  });
  return members;
}

ResponsePlan response_plan::build(const ResponsePlanInput& input)
{
  ResponsePlan plan{.environmentId = input.environmentId,
                    .strategy = ResponseStrategy::Ordered,
                    .entries = {},
                    .stepCount = 0,
                    .stepSeconds = input.stepSeconds,
                    .offerCamera = input.hasCamera,
                    .offerSiren = false,
                    .emergencyNumber = input.emergencyNumber,
                    .contacts = {input.contacts.begin(), input.contacts.end()}};
  for (const auto& member : input.members) {
    if (member.mode == RecipientMode::Off ||
        std::ranges::find(input.excluded, member.userId) !=
            input.excluded.end())
      continue;
    const bool mandatory =
        member.role == UserRole::Guard && (member.onDuty || input.staffedNow);
    plan.entries.push_back(
        {.userId = member.userId,
         .step = member.step,
         .mode = mandatory ? RecipientMode::Call : member.mode,
         .mandatory = mandatory,
         .discreet = false});
  }

  const PresenceSummary summary = summarize(plan.entries, input.presence);
  plan.strategy = strategyFor(input, summary);
  switch (plan.strategy) {
    case ResponseStrategy::Everyone:
      for (auto& entry : plan.entries)
        entry.step = 0;
      break;
    case ResponseStrategy::NightQuiet:
      for (auto& entry : plan.entries) {
        entry.step = 0;
        if (!entry.mandatory)
          entry.mode = RecipientMode::Notify;
      }
      break;
    case ResponseStrategy::InsideFirst:
      for (auto& entry : plan.entries) {
        if (isHome(input.presence, entry.userId)) {
          entry.step = 0;
          entry.discreet = true;
        }
        else {
          entry.step += 1;
        }
      }
      break;
    case ResponseStrategy::Ordered:
      break;
  }
  compactSteps(plan.entries);
  std::ranges::stable_sort(plan.entries, {}, &ResponsePlanEntry::step);
  plan.stepCount = plan.entries.empty() ? 0 : plan.entries.back().step + 1;
  plan.offerSiren = (input.trigger == ResponseTrigger::Intrusion ||
                     input.trigger == ResponseTrigger::Escalation) &&
                    summary.confirmedAway;
  return plan;
}

std::vector<int64_t> response_plan::stepUsers(const ResponsePlan& plan,
                                              int step)
{
  std::vector<int64_t> users;
  for (const auto& entry : plan.entries) {
    if (entry.step == step)
      users.push_back(entry.userId);
  }
  return users;
}

std::vector<int64_t> response_plan::audience(const AudienceInput& input)
{
  std::vector<int64_t> users;
  users.reserve(input.members.size());
  for (const auto& member : input.members) {
    if (member.mode != RecipientMode::Off &&
        std::ranges::find(input.excluded, member.userId) ==
            input.excluded.end())
      users.push_back(member.userId);
  }
  std::ranges::sort(users);
  return users;
}

Json::Value response_plan::toJson(const ResponsePlan& plan)
{
  Json::Value json(Json::objectValue);
  json["v"] = 1;
  json["environmentId"] = static_cast<Json::Int64>(plan.environmentId);
  json["strategy"] = responseStrategyToString(plan.strategy);
  json["stepCount"] = plan.stepCount;
  json["stepSeconds"] = plan.stepSeconds;
  Json::Value offers(Json::arrayValue);
  if (plan.offerCamera)
    offers.append("camera");
  if (plan.offerSiren)
    offers.append("siren");
  json["offers"] = std::move(offers);
  json["emergencyNumber"] = plan.emergencyNumber;
  Json::Value contacts(Json::arrayValue);
  for (const auto& contact : plan.contacts) {
    Json::Value item(Json::objectValue);
    item["name"] = contact.name;
    item["phone"] = contact.phone;
    item["note"] = contact.note;
    contacts.append(std::move(item));
  }
  json["contacts"] = std::move(contacts);
  Json::Value recipients(Json::arrayValue);
  for (const auto& entry : plan.entries) {
    Json::Value item(Json::objectValue);
    item["userId"] = static_cast<Json::Int64>(entry.userId);
    item["step"] = entry.step;
    item["mode"] = recipientModeToString(entry.mode);
    item["mandatory"] = entry.mandatory;
    item["discreet"] = entry.discreet;
    recipients.append(std::move(item));
  }
  json["recipients"] = std::move(recipients);
  return json;
}

bool response_plan::staffedAt(const GuardSchedule& schedule,
                              const std::tm& local)
{
  return schedule.enabled &&
         (guard_schedule::inWindows(schedule.staffed, local) ||
          guard_schedule::inWindows(schedule.open, local));
}

bool response_plan::clearThreat(const Json::Value& data)
{
  if (data.get("urgency", "").asString() == "critical")
    return true;
  for (const auto& reason : data["reasons"])
    if (reason.isString() && reason.asString() == "watchlist")
      return true;
  return false;
}
