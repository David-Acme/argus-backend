#pragma once

#include <json/value.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace turn::speech
{

enum class DateKind : unsigned char
{
  Today,
  Tomorrow,
  DayAfterTomorrow,
  Weekday,
  CalendarDate,
  Clock
};

struct DatePart
{
  DateKind kind{DateKind::CalendarDate};
  int64_t epoch{0};

  bool operator==(const DatePart& other) const { return kind == other.kind && epoch == other.epoch; }
};

enum class AskReason : unsigned char
{
  Missing,
  AmbiguousDate,
  DatePassed,
  BeyondRange,
  ProjectChoice,
  ProjectName,
  ProjectNoneYet
};

struct AskSlot
{
  std::string slot;
  std::string tool;
  Json::Value knownArgs{Json::objectValue};
  AskReason reason{AskReason::Missing};
  std::vector<DatePart> dates;
  std::vector<std::string> options;

  bool operator==(const AskSlot& other) const
  {
    return slot == other.slot && tool == other.tool && reason == other.reason && dates == other.dates &&
           options == other.options;
  }
};

struct Confirm
{
  std::string action;
  Json::Value args{Json::objectValue};
  bool irreversible{false};
  std::string toolPreview;
  std::string module;

  bool operator==(const Confirm& other) const
  {
    return action == other.action && irreversible == other.irreversible && toolPreview == other.toolPreview &&
           module == other.module;
  }
};

struct Choose
{
  std::vector<std::string> options;

  bool operator==(const Choose& other) const { return options == other.options; }
};

struct Done
{
  std::string tool;
  std::string fact;
  std::string readback;

  bool operator==(const Done& other) const
  {
    return tool == other.tool && fact == other.fact && readback == other.readback;
  }
};

struct Refused
{
  std::string tool;
  std::string reason;

  bool operator==(const Refused& other) const { return tool == other.tool && reason == other.reason; }
};

struct Offer
{
  std::string module;
  std::string name;
  std::string facts;
  std::string pendingIntent;

  bool operator==(const Offer& other) const
  {
    return module == other.module && name == other.name && facts == other.facts &&
           pendingIntent == other.pendingIntent;
  }
};

struct Declined
{
  bool operator==(const Declined&) const { return true; }
};

struct Unactionable
{
  std::string reason;

  bool operator==(const Unactionable& other) const { return reason == other.reason; }
};

struct Capability
{
  std::string fact;

  bool operator==(const Capability& other) const { return fact == other.fact; }
};

struct Misunderstood
{
  bool operator==(const Misunderstood&) const { return true; }
};

using Act = std::variant<AskSlot,
                         Confirm,
                         Choose,
                         Done,
                         Refused,
                         Offer,
                         Declined,
                         Unactionable,
                         Capability,
                         Misunderstood>;

struct Speech
{
  std::vector<Act> acts;
  std::string lang;
  int64_t now{0};
};

[[nodiscard]] std::string_view actName(const Act& act);

[[nodiscard]] bool isQuestion(const Act& act);

[[nodiscard]] std::string_view slotOf(const Act& act);

[[nodiscard]] std::string_view reasonName(AskReason reason);

}
