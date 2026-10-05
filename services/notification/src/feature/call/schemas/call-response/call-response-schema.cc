#include "call-response-schema.hxx"

#include <text/json-util.hxx>

#include <algorithm>
#include <ranges>

namespace
{
std::string text(const Json::Value& data, const char* key)
{
  const Json::Value& value = data[key];
  return value.isString() ? value.asString() : std::string{};
}

Json::Value person(int64_t userId, const std::string& name)
{
  if (userId <= 0)
    return {Json::nullValue};
  Json::Value json(Json::objectValue);
  json["userId"] = static_cast<Json::Int64>(userId);
  json["name"] = name;
  return json;
}

bool knownStrategy(const std::string& strategy)
{
  return strategy == "ordered" || strategy == "inside_first" ||
         strategy == "everyone" || strategy == "night_quiet";
}
}

CallResponseSchema CallResponseSchema::fromRow(const drogon::orm::Row& row)
{
  CallResponseSchema response{
      .id = row["id"].as<int64_t>(),
      .dedupeKey = row["dedupe_key"].as<std::string>(),
      .kind = row["kind"].as<std::string>(),
      .environmentId = row["environment_id"].as<int64_t>(),
      .episodeId = row["episode_id"].as<int64_t>(),
      .cameraId = row["camera_id"].as<int64_t>(),
      .strategy = row["strategy"].as<std::string>(),
      .state = responseStateFromString(row["state"].as<std::string>())
                   .value_or(ResponseState::Expired),
      .step = row["step"].as<int>(),
      .stepCount = row["step_count"].as<int>(),
      .stepSeconds = row["step_seconds"].as<int>(),
      .stepDeadline = row["step_deadline"].as<int64_t>(),
      .responderId = row["responder_id"].as<int64_t>(),
      .responderName = row["responder_name"].as<std::string>(),
      .verdict = responseVerdictFromString(row["verdict"].as<std::string>()),
      .verdictBy = row["verdict_by"].as<int64_t>(),
      .verdictByName = row["verdict_by_name"].as<std::string>(),
      .verdictAt = row["verdict_at"].as<int64_t>(),
      .plan = json_util::fromString(row["plan"].as<std::string>()),
      .data = json_util::fromString(row["data"].as<std::string>()),
      .createdAt = row["created_at"].as<int64_t>(),
      .updatedAt = row["updated_at"].as<int64_t>()};
  return response;
}

CallResponseMember CallResponseMember::fromRow(const drogon::orm::Row& row)
{
  return {.responseId = row["response_id"].as<int64_t>(),
          .userId = row["user_id"].as<int64_t>(),
          .step = row["step"].as<int>(),
          .mode = responseMemberModeFromString(row["mode"].as<std::string>()),
          .mandatory = row["mandatory"].as<int64_t>() != 0,
          .discreet = row["discreet"].as<int64_t>() != 0,
          .reachedAt = row["reached_at"].as<int64_t>()};
}

std::optional<ParsedResponsePlan> call_response::parsePlan(const Json::Value& plan)
{
  if (!plan.isObject() || !plan["recipients"].isArray())
    return std::nullopt;
  ParsedResponsePlan parsed{.strategy = text(plan, "strategy"),
                            .stepCount = 0,
                            .stepSeconds = plan.get("stepSeconds", 45).asInt(),
                            .entries = {}};
  if (!knownStrategy(parsed.strategy))
    parsed.strategy = "ordered";
  parsed.stepSeconds = std::clamp(parsed.stepSeconds, kMinStepSeconds, kMaxStepSeconds);
  for (const auto& item : plan["recipients"]) {
    if (!item.isObject() || !item["userId"].isIntegral() || !item["step"].isIntegral())
      continue;
    const int64_t userId = item["userId"].asInt64();
    const int step = item["step"].asInt();
    if (userId <= 0 || step < 0 || step >= kMaxSteps ||
        std::ranges::find(parsed.entries, userId, &ResponsePlanEntry::userId) !=
            parsed.entries.end())
      continue;
    parsed.entries.push_back(
        {.userId = userId,
         .step = step,
         .mode = responseMemberModeFromString(text(item, "mode")),
         .mandatory = item.get("mandatory", false).asBool(),
         .discreet = item.get("discreet", false).asBool()});
  }
  if (parsed.entries.empty())
    return std::nullopt;
  for (const auto& entry : parsed.entries)
    parsed.stepCount = std::max(parsed.stepCount, entry.step + 1);
  return parsed;
}

std::string call_response::responseId(int64_t id)
{
  return "response-" + std::to_string(id);
}

Json::Value call_response::toJson(const ViewInput& input)
{
  const CallResponseSchema& response = input.response;
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(response.id);
  json["threadKey"] = response.dedupeKey;
  json["kind"] = response.kind;
  json["environmentId"] = static_cast<Json::Int64>(response.environmentId);
  json["environmentName"] = text(response.data, "environmentName");
  json["cameraId"] = static_cast<Json::Int64>(response.cameraId);
  json["cameraName"] = text(response.data, "cameraName");
  json["episodeId"] = static_cast<Json::Int64>(response.episodeId);
  json["strategy"] = response.strategy;
  json["state"] = responseStateToString(response.state);
  json["step"] = response.step;
  json["stepCount"] = response.stepCount;
  json["attendedBy"] = person(response.responderId, response.responderName);
  json["verdict"] = response.verdict ? responseVerdictToString(*response.verdict)
                                     : std::string{};
  json["verdictBy"] = person(response.verdictBy, response.verdictByName);
  json["verdictAt"] = static_cast<Json::Int64>(response.verdictAt);
  json["emergencyNumber"] = text(response.plan, "emergencyNumber");
  json["contacts"] = response.plan["contacts"].isArray()
                         ? response.plan["contacts"]
                         : Json::Value(Json::arrayValue);
  json["showContacts"] = response.state == ResponseState::Unanswered ||
                         response.state == ResponseState::Confirmed;
  json["offers"] = response.plan["offers"].isArray() ? response.plan["offers"]
                                                     : Json::Value(Json::arrayValue);
  json["createdAt"] = static_cast<Json::Int64>(response.createdAt);
  json["updatedAt"] = static_cast<Json::Int64>(response.updatedAt);
  if (input.member != nullptr) {
    Json::Value mine(Json::objectValue);
    mine["step"] = input.member->step;
    mine["mode"] = responseMemberModeToString(input.member->mode);
    mine["mandatory"] = input.member->mandatory;
    mine["discreet"] = input.member->discreet;
    mine["reached"] = input.member->reachedAt > 0;
    json["mine"] = std::move(mine);
  }
  else {
    json["mine"] = Json::Value(Json::nullValue);
  }
  return json;
}
