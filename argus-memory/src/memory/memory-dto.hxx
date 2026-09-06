#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>

// Caller context of the internal wire (mirrors tools::ToolContext).
struct MemoryToolContext
{
  int64_t userId{0};
  std::string lang{"es"};
  std::string sessionId;
};

// POST /memory/v1/remember body: mirrors the memory.remember descriptor
// parameters (Ruling BY).
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

// POST /memory/v1/recall body: mirrors the memory.recall descriptor.
struct RecallBody
{
  std::string query;
  MemoryToolContext context;

  static RecallBody fromJson(const Json::Value& json);
};

// POST /memory/v1/forget body: mirrors the memory.forget descriptor.
struct ForgetBody
{
  int64_t factId{0};

  static ForgetBody fromJson(const Json::Value& json);
};

// POST /memory/v1/procedure-run body: mirrors the procedure.run descriptor.
struct ProcedureBody
{
  std::string goal;

  static ProcedureBody fromJson(const Json::Value& json);
};

// POST /memory/v1/capture body: the ConversationService capture surface.
struct CaptureBody
{
  std::string text;
  std::string lang{"es"};
  int64_t userId{0};

  static CaptureBody fromJson(const Json::Value& json);
};

// POST /memory/v1/compact body: the enqueueCompaction surface.
struct CompactBody
{
  int64_t userId{0};
  std::string transcript;
  std::string lang{"es"};

  static CompactBody fromJson(const Json::Value& json);
};

// POST /memory/v1/durable-transcript body: the probe-asserted surface.
struct DurableTranscriptBody
{
  std::string transcript;
  std::string lang{"es"};

  static DurableTranscriptBody fromJson(const Json::Value& json);
};
