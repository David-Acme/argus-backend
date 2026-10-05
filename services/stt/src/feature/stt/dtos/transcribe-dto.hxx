#pragma once

#include <drogon/HttpRequest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

inline constexpr std::size_t kMaxTranscribeSeconds = 120;

struct TranscribeDto
{
  std::string_view body;
  std::string lang;

  static TranscribeDto fromRequest(const drogon::HttpRequestPtr& req);

  [[nodiscard]] std::vector<float> samples() const;
};
