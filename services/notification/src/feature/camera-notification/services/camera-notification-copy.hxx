#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

enum class FallbackNoticeKind : uint8_t
{
  Alert = 0,
  Digest
};

struct FallbackNotice
{
  FallbackNoticeKind kind{FallbackNoticeKind::Alert};
  int64_t cameraId{0};
  std::string cameraName;
  std::string rule;
  std::map<std::string, int> suppressed;
};

struct FallbackText
{
  std::string title;
  std::string body;
};

namespace camera_notification_copy
{

struct LangPreference
{
  std::string_view requested;
  std::string_view fallback;
};

std::string normalizeLang(const LangPreference& preference);

FallbackText render(const FallbackNotice& notice, std::string_view lang);

}
