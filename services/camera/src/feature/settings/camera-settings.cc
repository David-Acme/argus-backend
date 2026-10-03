#include "camera-settings.hxx"

namespace
{

struct NumericSetting
{
  const char* key;
  const char* group;
  SettingLevel level{SettingLevel::Advanced};
  SettingApply apply{SettingApply::Restart};
  SettingRange range{};
  const char* fallback;
};

struct ToggleSetting
{
  const char* key;
  const char* group;
  SettingLevel level{SettingLevel::Advanced};
  SettingApply apply{SettingApply::Restart};
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

SettingSpec toggle(const ToggleSetting& setting)
{
  return {.key = setting.key,
          .group = setting.group,
          .type = SettingType::Toggle,
          .level = setting.level,
          .apply = setting.apply,
          .range = {},
          .choices = {},
          .fallback = setting.fallback};
}

}

std::vector<SettingSpec> cameraSettingsCatalog()
{
  return {
      toggle({.key = "objects.enabled",
              .group = "detection",
              .level = SettingLevel::Basic,
              .apply = SettingApply::Restart,
              .fallback = "false"}),
      decimal({.key = "objects.conf",
               .group = "detection",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Restart,
               .range = {.min = 0.2, .max = 0.9, .step = 0.05},
               .fallback = "0.45"}),
      integer({.key = "operator.night_start",
               .group = "alerts",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Restart,
               .range = {.min = 0, .max = 23, .step = 1},
               .fallback = "22"}),
      integer({.key = "operator.night_end",
               .group = "alerts",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Restart,
               .range = {.min = 0, .max = 23, .step = 1},
               .fallback = "6"}),
      integer({.key = "operator.cooldown_ms",
               .group = "alerts",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 1000, .max = 600000, .step = 1000},
               .fallback = "30000"}),
      integer({.key = "operator.person_recheck_ms",
               .group = "alerts",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 5000, .max = 600000, .step = 1000},
               .fallback = "30000"}),
      toggle({.key = "actions.enabled",
              .group = "actions",
              .level = SettingLevel::Basic,
              .apply = SettingApply::Live,
              .fallback = "false"}),
      integer({.key = "streaming.max_viewers_per_camera",
               .group = "streaming",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 1, .max = 16, .step = 1},
               .fallback = "4"}),
      integer({.key = "streaming.max_total_viewers",
               .group = "streaming",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 1, .max = 64, .step = 1},
               .fallback = "8"}),
      integer({.key = "streaming.hub_window_bytes",
               .group = "streaming",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::NextSession,
               .range = {.min = 32768, .max = 1048576, .step = 16384},
               .fallback = "131072"}),
      toggle({.key = "health.enabled",
              .group = "health",
              .level = SettingLevel::Basic,
              .apply = SettingApply::Restart,
              .fallback = "true"}),
      integer({.key = "health.interval_ms",
               .group = "health",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 10000, .max = 3600000, .step = 1000},
               .fallback = "60000"}),
      decimal({.key = "health.dark_threshold",
               .group = "health",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 1, .max = 127, .step = 1},
               .fallback = "25"}),
      decimal({.key = "health.bright_threshold",
               .group = "health",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 128, .max = 254, .step = 1},
               .fallback = "235"}),
      decimal({.key = "health.blur_threshold",
               .group = "health",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 1, .max = 200, .step = 1},
               .fallback = "18"}),
      decimal({.key = "health.scene_diff",
               .group = "health",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 0.05, .max = 0.95, .step = 0.05},
               .fallback = "0.35"}),
      integer({.key = "health.rebaseline_after_s",
               .group = "health",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 60, .max = 86400, .step = 60},
               .fallback = "900"}),
  };
}
