#pragma once

#include <cstddef>
#include <drogon/MultiPart.h>
#include <string>

struct VoiceSampleDto
{
  static constexpr size_t kMaxBytes = size_t{6} * 1024 * 1024;

  std::string sample;

  static VoiceSampleDto form_multipart(const drogon::MultiPartParser& parser);
};
