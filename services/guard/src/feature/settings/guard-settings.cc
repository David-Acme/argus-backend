#include "guard-settings.hxx"

namespace
{

struct ValueSetting
{
  const char* key;
  const char* group;
  SettingLevel level{SettingLevel::Advanced};
  SettingRange range{};
  const char* fallback;
};

struct ChoiceSetting
{
  const char* key;
  const char* group;
  SettingLevel level{SettingLevel::Advanced};
  std::vector<std::string> choices;
  const char* fallback;
};

SettingSpec live(const ValueSetting& setting, SettingType type)
{
  return {.key = setting.key,
          .group = setting.group,
          .type = type,
          .level = setting.level,
          .apply = SettingApply::Live,
          .range = setting.range,
          .choices = {},
          .fallback = setting.fallback};
}

SettingSpec integer(const ValueSetting& setting)
{
  return live(setting, SettingType::Integer);
}

SettingSpec decimal(const ValueSetting& setting)
{
  return live(setting, SettingType::Decimal);
}

SettingSpec toggle(const ValueSetting& setting)
{
  return live(setting, SettingType::Toggle);
}

SettingSpec choice(const ChoiceSetting& setting)
{
  return {.key = setting.key,
          .group = setting.group,
          .type = SettingType::Choice,
          .level = setting.level,
          .apply = SettingApply::Live,
          .range = {},
          .choices = setting.choices,
          .fallback = setting.fallback};
}

}

std::vector<SettingSpec> guardSettingsCatalog()
{
  return {
      integer({.key = "guard.notify_level",
               .group = "response",
               .level = SettingLevel::Basic,
               .range = {.min = 1, .max = 4, .step = 1},
               .fallback = "2"}),
      integer({.key = "guard.announce_level",
               .group = "response",
               .level = SettingLevel::Basic,
               .range = {.min = 1, .max = 5, .step = 1},
               .fallback = "3"}),
      integer({.key = "guard.alarm_level",
               .group = "response",
               .level = SettingLevel::Basic,
               .range = {.min = 1, .max = 5, .step = 1},
               .fallback = "4"}),
      integer({.key = "guard.alarm_seconds",
               .group = "alarm",
               .level = SettingLevel::Basic,
               .range = {.min = 1, .max = 60, .step = 1},
               .fallback = "6"}),
      toggle({.key = "guard.arm_siren",
              .group = "alarm",
              .level = SettingLevel::Basic,
              .range = {},
              .fallback = "false"}),
      integer({.key = "guard.siren_seconds",
               .group = "alarm",
               .level = SettingLevel::Basic,
               .range = {.min = 1, .max = 300, .step = 1},
               .fallback = "20"}),
      toggle({.key = "guard.greet_enabled",
              .group = "visitors",
              .level = SettingLevel::Basic,
              .range = {},
              .fallback = "true"}),
      toggle({.key = "guard.greet_known",
              .group = "visitors",
              .level = SettingLevel::Basic,
              .range = {},
              .fallback = "false"}),
      toggle({.key = "guard.expected_guests",
              .group = "visitors",
              .level = SettingLevel::Basic,
              .range = {},
              .fallback = "true"}),
      toggle({.key = "guard.quiet_hours.enabled",
              .group = "quiet",
              .level = SettingLevel::Basic,
              .range = {},
              .fallback = "false"}),
      integer({.key = "guard.quiet_hours.start_hour",
               .group = "quiet",
               .level = SettingLevel::Basic,
               .range = {.min = 0, .max = 23, .step = 1},
               .fallback = "22"}),
      integer({.key = "guard.quiet_hours.end_hour",
               .group = "quiet",
               .level = SettingLevel::Basic,
               .range = {.min = 0, .max = 23, .step = 1},
               .fallback = "7"}),
      integer({.key = "guard.quiet_hours.daily_budget",
               .group = "quiet",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 500, .step = 1},
               .fallback = "30"}),
      choice({.key = "guard.decision_mode",
              .group = "decisions",
              .level = SettingLevel::Advanced,
              .choices = {"shadow", "enforce"},
              .fallback = "shadow"}),
      choice({.key = "guard.belief.gate_scope",
              .group = "decisions",
              .level = SettingLevel::Advanced,
              .choices = {"notify", "communication", "all"},
              .fallback = "notify"}),
      integer({.key = "guard.belief.threshold_critical",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = -12, .max = 12, .step = 1},
               .fallback = "1"}),
      integer({.key = "guard.belief.threshold_high",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = -12, .max = 12, .step = 1},
               .fallback = "3"}),
      integer({.key = "guard.belief.threshold_medium",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = -12, .max = 12, .step = 1},
               .fallback = "5"}),
      integer({.key = "guard.belief.threshold_low",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = -12, .max = 12, .step = 1},
               .fallback = "7"}),
      decimal({.key = "guard.belief.detector_strong",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = 0.3, .max = 0.99, .step = 0.01},
               .fallback = "0.75"}),
      decimal({.key = "guard.belief.detector_weak",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = 0.05, .max = 0.9, .step = 0.01},
               .fallback = "0.35"}),
      integer({.key = "guard.belief.zone_dwell_alert_ms",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = 500, .max = 120000, .step = 500},
               .fallback = "3000"}),
      integer({.key = "guard.belief.zone_dwell_monitor_ms",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = 500, .max = 300000, .step = 500},
               .fallback = "12000"}),
      integer({.key = "guard.belief_refresh_s",
               .group = "decisions",
               .level = SettingLevel::Advanced,
               .range = {.min = 10, .max = 3600, .step = 10},
               .fallback = "300"}),
      integer({.key = "guard.action_cooldown_s",
               .group = "limits",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 86400, .step = 1},
               .fallback = "120"}),
      integer({.key = "guard.repeat_window_s",
               .group = "limits",
               .level = SettingLevel::Advanced,
               .range = {.min = 60, .max = 604800, .step = 60},
               .fallback = "86400"}),
      integer({.key = "guard.regroup_window_s",
               .group = "limits",
               .level = SettingLevel::Advanced,
               .range = {.min = 0, .max = 86400, .step = 60},
               .fallback = "600"}),
      integer({.key = "guard.max_actions_per_hour",
               .group = "limits",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 60, .step = 1},
               .fallback = "4"}),
      integer({.key = "guard.max_dialogue_turns",
               .group = "dialogue",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 10, .step = 1},
               .fallback = "3"}),
      integer({.key = "guard.greet_listen_seconds",
               .group = "dialogue",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 10, .step = 1},
               .fallback = "6"}),
      toggle({.key = "guard.greet_reply_enabled",
              .group = "dialogue",
              .level = SettingLevel::Advanced,
              .range = {},
              .fallback = "true"}),
      integer({.key = "guard.cross_camera_window_s",
               .group = "tracking",
               .level = SettingLevel::Advanced,
               .range = {.min = 5, .max = 600, .step = 5},
               .fallback = "60"}),
      integer({.key = "guard.continuity_window_s",
               .group = "tracking",
               .level = SettingLevel::Advanced,
               .range = {.min = 5, .max = 600, .step = 5},
               .fallback = "45"}),
      decimal({.key = "guard.signature_min_similarity",
               .group = "tracking",
               .level = SettingLevel::Advanced,
               .range = {.min = 0.5, .max = 0.99, .step = 0.01},
               .fallback = "0.82"}),
      integer({.key = "guard.loiter_checks",
               .group = "tracking",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 20, .step = 1},
               .fallback = "3"}),
      toggle({.key = "guard.staging",
              .group = "tracking",
              .level = SettingLevel::Advanced,
              .range = {},
              .fallback = "true"}),
      integer({.key = "guard.encounter_timeout_s",
               .group = "tracking",
               .level = SettingLevel::Advanced,
               .range = {.min = 30, .max = 3600, .step = 30},
               .fallback = "300"}),
      integer({.key = "guard.tamper_sustained_s",
               .group = "health",
               .level = SettingLevel::Advanced,
               .range = {.min = 30, .max = 3600, .step = 30},
               .fallback = "300"}),
      integer({.key = "guard.offline_sustained_s",
               .group = "health",
               .level = SettingLevel::Advanced,
               .range = {.min = 60, .max = 3600, .step = 30},
               .fallback = "240"}),
      integer({.key = "guard.health_stale_s",
               .group = "health",
               .level = SettingLevel::Advanced,
               .range = {.min = 30, .max = 3600, .step = 30},
               .fallback = "300"}),
      integer({.key = "guard.journal_retention_days",
               .group = "history",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 60, .step = 1},
               .fallback = "30"}),
      integer({.key = "guard.marked_retention_days",
               .group = "history",
               .level = SettingLevel::Advanced,
               .range = {.min = 30, .max = 120, .step = 1},
               .fallback = "120"}),
  };
}
