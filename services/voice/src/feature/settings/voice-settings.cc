#include "voice-settings.hxx"

namespace
{

struct NumericSetting
{
  const char* key;
  const char* group;
  SettingLevel level{SettingLevel::Advanced};
  SettingRange range{};
  const char* fallback;
};

SettingSpec integer(const NumericSetting& setting)
{
  return {.key = setting.key,
          .group = setting.group,
          .type = SettingType::Integer,
          .level = setting.level,
          .apply = SettingApply::NextSession,
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
          .apply = SettingApply::NextSession,
          .range = setting.range,
          .choices = {},
          .fallback = setting.fallback};
}

}

std::vector<SettingSpec> voiceSettingsCatalog()
{
  return {
      {.key = "stt.language",
       .group = "conversation",
       .type = SettingType::Choice,
       .level = SettingLevel::Basic,
       .apply = SettingApply::NextSession,
       .range = {},
       .choices = {"es", "en"},
       .fallback = "es"},
      decimal({.key = "vad.barge_threshold",
               .group = "conversation",
               .level = SettingLevel::Basic,
               .range = {.min = 0.3, .max = 0.95, .step = 0.05},
               .fallback = "0.7"}),
      integer({.key = "vad.min_silence_frames",
               .group = "conversation",
               .level = SettingLevel::Basic,
               .range = {.min = 4, .max = 60, .step = 1},
               .fallback = "12"}),
      decimal({.key = "vad.threshold",
               .group = "listening",
               .level = SettingLevel::Advanced,
               .range = {.min = 0.1, .max = 0.95, .step = 0.05},
               .fallback = "0.45"}),
      decimal({.key = "vad.neg_threshold",
               .group = "listening",
               .level = SettingLevel::Advanced,
               .range = {.min = 0.05, .max = 0.9, .step = 0.05},
               .fallback = "0.25"}),
      integer({.key = "vad.min_speech_frames",
               .group = "listening",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 30, .step = 1},
               .fallback = "5"}),
      integer({.key = "vad.max_turn_frames",
               .group = "listening",
               .level = SettingLevel::Advanced,
               .range = {.min = 100, .max = 1500, .step = 25},
               .fallback = "750"}),
      integer({.key = "vad.pre_roll_frames",
               .group = "listening",
               .level = SettingLevel::Advanced,
               .range = {.min = 1, .max = 30, .step = 1},
               .fallback = "10"}),
      integer({.key = "vad.min_turn_ms",
               .group = "listening",
               .level = SettingLevel::Advanced,
               .range = {.min = 50, .max = 2000, .step = 10},
               .fallback = "240"}),
      decimal({.key = "vad.min_mean_prob",
               .group = "listening",
               .level = SettingLevel::Advanced,
               .range = {.min = 0.05, .max = 0.95, .step = 0.05},
               .fallback = "0.35"}),
      integer({.key = "vad.barge_min_frames",
               .group = "conversation",
               .level = SettingLevel::Advanced,
               .range = {.min = 2, .max = 40, .step = 1},
               .fallback = "8"}),
      integer({.key = "vad.barge_guard_ms",
               .group = "conversation",
               .level = SettingLevel::Advanced,
               .range = {.min = 50, .max = 3000, .step = 50},
               .fallback = "300"}),
      {.key = "vad.denoise",
       .group = "noise",
       .type = SettingType::Toggle,
       .level = SettingLevel::Advanced,
       .apply = SettingApply::NextSession,
       .range = {},
       .choices = {},
       .fallback = "true"},
      decimal({.key = "vad.denoise_gate_rms",
               .group = "noise",
               .level = SettingLevel::Advanced,
               .range = {.min = 0.0005, .max = 0.05, .step = 0.0005},
               .fallback = "0.0035"}),
  };
}
