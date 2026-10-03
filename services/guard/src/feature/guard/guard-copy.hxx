#pragma once

#include <feature/guard/vocabulary/camera-role.hxx>
#include <feature/guard/vocabulary/guard-danger.hxx>
#include <feature/guard/vocabulary/guard-reason.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class NoticeKind : uint8_t
{
  Episode = 0,
  Escalation,
  Tamper,
  Digest
};

enum class NoticeSubject : uint8_t
{
  Stranger = 0,
  Unobserved,
  Several,
  Accompanied
};

enum class NoticeAction : uint8_t
{
  Watching = 0,
  Speaker,
  Alarm,
  Greeted,
  GreetedNoReply,
  SilentWeapon
};

std::string noticeKindToString(NoticeKind kind);
std::string noticeSubjectToString(NoticeSubject subject);
std::string noticeActionToString(NoticeAction action);

struct DigestLine
{
  int64_t cameraId{0};
  std::string cameraName;
  int64_t count{0};
};

struct GuardNotice
{
  NoticeKind kind{NoticeKind::Episode};
  NoticeSubject subject{NoticeSubject::Stranger};
  int people{1};
  int64_t cameraId{0};
  std::string cameraName;
  CameraRole role{CameraRole::Other};
  bool outdoor{false};
  std::string zoneName;
  std::vector<GuardReason> reasons;
  int64_t dwellS{0};
  GuardDanger danger{GuardDanger::None};
  NoticeAction action{NoticeAction::Watching};
  std::string tamperStatus;
  std::vector<DigestLine> held;
  std::vector<DigestLine> routine;
  int64_t notified{0};
  bool afterQuiet{false};
};

struct NoticeText
{
  std::string title;
  std::string body;
};

namespace guard_copy
{

struct LangPreference
{
  std::string_view requested;
  std::string_view fallback;
};

std::string normalizeLang(const LangPreference& preference);

NoticeText render(const GuardNotice& notice, std::string_view lang);

std::string urgency(const GuardNotice& notice);

}
