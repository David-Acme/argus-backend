#include "response-recording-state-dto.hxx"

namespace
{
constexpr int64_t kBytesPerGb = 1024LL * 1024 * 1024;
}

ResponseRecordingStateDto
ResponseRecordingStateDto::fromState(const RecordingState& state)
{
  return {.paused = state.paused,
          .freeGb = state.freeBytes / kBytesPerGb,
          .minFreeGb = state.minFreeBytes / kBytesPerGb,
          .freeBytes = state.freeBytes};
}

Json::Value ResponseRecordingStateDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["paused"] = paused;
  json["freeGb"] = static_cast<Json::Int64>(freeGb);
  json["minFreeGb"] = static_cast<Json::Int64>(minFreeGb);
  json["freeBytes"] = static_cast<Json::Int64>(freeBytes);
  return json;
}
