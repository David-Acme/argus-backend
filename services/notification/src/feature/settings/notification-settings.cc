#include "notification-settings.hxx"

namespace
{

struct NumericSetting
{
  const char* key;
  const char* group;
  SettingLevel level{SettingLevel::Advanced};
  SettingApply apply{SettingApply::Live};
  SettingRange range{};
  const char* fallback;
};

SettingSpec integer(const NumericSetting& setting)
{
  return {.key = setting.key,
          .group = setting.group,
          .type = SettingType::Integer,
          .level = setting.level,
          .apply = setting.apply,
          .range = setting.range,
          .choices = {},
          .fallback = setting.fallback};
}

SettingSpec decimal(const NumericSetting& setting)
{
  return {.key = setting.key,
          .group = setting.group,
          .type = SettingType::Decimal,
          .level = setting.level,
          .apply = setting.apply,
          .range = setting.range,
          .choices = {},
          .fallback = setting.fallback};
}

}

std::vector<SettingSpec> notificationSettingsCatalog()
{
  return {
      integer({.key = "notifications.budget_per_hour",
               .group = "alerts",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Live,
               .range = {.min = 1, .max = 60, .step = 1},
               .fallback = "6"}),
      {.key = "notifications.fallback_suppress_known",
       .group = "alerts",
       .type = SettingType::Toggle,
       .level = SettingLevel::Basic,
       .apply = SettingApply::Live,
       .range = {},
       .choices = {},
       .fallback = "true"},
      decimal({.key = "notifications.fallback_min_score_median",
               .group = "alerts",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 0.05, .max = 0.95, .step = 0.05},
               .fallback = "0.3"}),
      integer({.key = "notifications.fallback_min_dwell_ms",
               .group = "alerts",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 100, .max = 60000, .step = 100},
               .fallback = "1000"}),
      integer({.key = "notifications.guard_heartbeat_timeout_s",
               .group = "alerts",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 5, .max = 600, .step = 5},
               .fallback = "30"}),
      integer({.key = "notifications.silent_start",
               .group = "quiet",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Live,
               .range = {.min = -1, .max = 23, .step = 1},
               .fallback = "-1"}),
      integer({.key = "notifications.silent_end",
               .group = "quiet",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Live,
               .range = {.min = -1, .max = 23, .step = 1},
               .fallback = "-1"}),
      integer({.key = "notifications.ack_window_s",
               .group = "delivery",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 3600, .max = 604800, .step = 3600},
               .fallback = "86400"}),
      integer({.key = "notifications.selftest_interval_s",
               .group = "delivery",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 0, .max = 3600, .step = 30},
               .fallback = "300"}),
      integer({.key = "notifications.fallback_retention_days",
               .group = "history",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 1, .max = 3650, .step = 1},
               .fallback = "90"}),
      {.key = "calls.enabled",
       .group = "calls",
       .type = SettingType::Toggle,
       .level = SettingLevel::Basic,
       .apply = SettingApply::Live,
       .range = {},
       .choices = {},
       .fallback = "true"},
      integer({.key = "calls.call_gap_s",
               .group = "calls",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 30, .max = 3600, .step = 30},
               .fallback = "300"}),
      integer({.key = "calls.max_calls_per_hour",
               .group = "calls",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 1, .max = 20, .step = 1},
               .fallback = "4"}),
      integer({.key = "calls.arrival_absence_s",
               .group = "calls",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 600, .max = 86400, .step = 600},
               .fallback = "10800"}),
  };
}
