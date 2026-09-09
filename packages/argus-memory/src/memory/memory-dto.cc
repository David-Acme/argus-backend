#include "memory-dto.hxx"

#include <shared/validation/validator.hxx>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace
{

constexpr size_t kMaxTextLength = 32 * 1024;

struct RequireTextInput
{
  const std::string& value;
  const char* field;
  ValidationErrors& errors;
};

void requireText(const RequireTextInput& input)
{
  const std::string& value = input.value;
  const char* field = input.field;
  if (value.empty())
    input.errors[field].push_back(std::string(field) + " is required");
  else if (value.size() > kMaxTextLength)
    input.errors[field].push_back(std::string(field) + " is too long");
}

MemoryToolContext contextFromJson(const Json::Value& json)
{
  MemoryToolContext context;
  const Json::Value& ctx = json["context"];
  if (ctx.isObject()) {
    if (ctx.isMember("user_id") && ctx["user_id"].isInt64())
      context.userId = ctx["user_id"].asInt64();
    if (ctx.isMember("lang") && ctx["lang"].isString())
      context.lang = ctx["lang"].asString();
    if (ctx.isMember("session_id") && ctx["session_id"].isString())
      context.sessionId = ctx["session_id"].asString();
  }
  return context;
}

} // namespace

RememberBody RememberBody::fromJson(const Json::Value& json)
{
  RememberBody body;
  body.subject = json.get("subject", "").asString();
  body.predicate = json.get("predicate", "").asString();
  body.value = json.get("value", "").asString();
  body.type = json.get("type", "").asString();
  if (json.isMember("confidence") && json["confidence"].isNumeric())
    body.confidence = json["confidence"].asDouble();
  body.context = contextFromJson(json);

  ValidationErrors errors;
  requireText({.value = body.subject, .field = "subject", .errors = errors});
  requireText({.value = body.predicate, .field = "predicate", .errors = errors});
  requireText({.value = body.value, .field = "value", .errors = errors});
  constexpr std::array<const char*, 5> kTypes = {"persona", "preference",
                                                 "schedule", "instruction",
                                                 "attribute"};
  if (std::find_if(kTypes.begin(), kTypes.end(),
                   [&](const char* type) { return body.type == type; }) ==
      kTypes.end())
    errors["type"].push_back("type must be persona, preference, schedule, "
                             "instruction or attribute");
  if (!errors.empty())
    throw ValidationException(errors);
  return body;
}

RecallBody RecallBody::fromJson(const Json::Value& json)
{
  RecallBody body;
  body.query = json.get("query", "").asString();
  body.context = contextFromJson(json);

  ValidationErrors errors;
  requireText({.value = body.query, .field = "query", .errors = errors});
  if (!errors.empty())
    throw ValidationException(errors);
  return body;
}

ForgetBody ForgetBody::fromJson(const Json::Value& json)
{
  ForgetBody body;
  body.factId = json.get("fact_id", 0).asInt64();

  ValidationErrors errors;
  if (body.factId <= 0)
    errors["fact_id"].push_back("fact_id must be a positive fact id");
  if (!errors.empty())
    throw ValidationException(errors);
  return body;
}

ProcedureBody ProcedureBody::fromJson(const Json::Value& json)
{
  ProcedureBody body;
  body.goal = json.get("goal", "").asString();

  ValidationErrors errors;
  requireText({.value = body.goal, .field = "goal", .errors = errors});
  if (!errors.empty())
    throw ValidationException(errors);
  return body;
}

CaptureBody CaptureBody::fromJson(const Json::Value& json)
{
  CaptureBody body;
  body.text = json.get("text", "").asString();
  if (json.isMember("lang") && json["lang"].isString())
    body.lang = json["lang"].asString();
  if (json.isMember("user_id") && json["user_id"].isInt64())
    body.userId = json["user_id"].asInt64();

  ValidationErrors errors;
  requireText({.value = body.text, .field = "text", .errors = errors});
  if (!errors.empty())
    throw ValidationException(errors);
  return body;
}

CompactBody CompactBody::fromJson(const Json::Value& json)
{
  CompactBody body;
  if (json.isMember("user_id") && json["user_id"].isInt64())
    body.userId = json["user_id"].asInt64();
  body.transcript = json.get("transcript", "").asString();
  if (json.isMember("lang") && json["lang"].isString())
    body.lang = json["lang"].asString();

  ValidationErrors errors;
  if (body.userId < 0)
    errors["user_id"].push_back("user_id must not be negative");
  requireText({.value = body.transcript, .field = "transcript", .errors = errors});
  if (!errors.empty())
    throw ValidationException(errors);
  return body;
}

DurableTranscriptBody DurableTranscriptBody::fromJson(const Json::Value& json)
{
  DurableTranscriptBody body;
  body.transcript = json.get("transcript", "").asString();
  if (json.isMember("lang") && json["lang"].isString())
    body.lang = json["lang"].asString();

  ValidationErrors errors;
  requireText({.value = body.transcript, .field = "transcript", .errors = errors});
  if (!errors.empty())
    throw ValidationException(errors);
  return body;
}
