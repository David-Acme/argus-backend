#pragma once

#include <cstdint>
#include <feature/recording/services/clip-recorder.hxx>
#include <json/value.h>

struct ResponseRecordingStateDto
{
  bool paused{false};
  int64_t freeGb{0};
  int64_t minFreeGb{0};
  int64_t freeBytes{0};

  static ResponseRecordingStateDto fromState(const RecordingState& state);
  Json::Value toJson() const;
};
