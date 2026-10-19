#pragma once

#include <cstdint>

struct MessengerSettings
{
  bool enabled{true};
  int64_t replyWindowSeconds{300};
  int64_t maxRelaysPerHour{20};
  int64_t maxMessageChars{500};
  int64_t maxVisitorNoticesPerHourPerCamera{6};
  int64_t retentionDays{30};
};
