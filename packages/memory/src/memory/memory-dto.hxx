#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>

struct MemoryToolContext
{
  int64_t userId{0};
  std::string lang{"es"};
  std::string sessionId;
};

struct RememberBody
{
  std::string subject;
  std::string predicate;
  std::string value;
  std::string type;
  std::optional<double> confidence;
  MemoryToolContext context;

  static RememberBody fromJson(const Json::Value& json);
};

struct RecallBody
{
  std::string query;
  MemoryToolContext context;

  static RecallBody fromJson(const Json::Value& json);
};

struct ForgetBody
{
  int64_t factId{0};

  static ForgetBody fromJson(const Json::Value& json);
};

struct ProcedureBody
{
  std::string goal;

  static ProcedureBody fromJson(const Json::Value& json);
};

struct CaptureBody
{
  std::string text;
  std::string lang{"es"};
  int64_t userId{0};

  static CaptureBody fromJson(const Json::Value& json);
};

struct CompactBody
{
  int64_t userId{0};
  std::string transcript;
  std::string lang{"es"};

  static CompactBody fromJson(const Json::Value& json);
};

struct DurableTranscriptBody
{
  std::string transcript;
  std::string lang{"es"};

  static DurableTranscriptBody fromJson(const Json::Value& json);
};
