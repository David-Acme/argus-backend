#include "tts-settings.hxx"

std::vector<SettingSpec> ttsSettingsCatalog()
{
  const auto integer = [](const char* key, SettingLevel level, SettingApply apply, SettingRange range, const char* fallback) {
    return SettingSpec{.key = key,
                       .group = "synthesis",
                       .type = SettingType::Integer,
                       .level = level,
                       .apply = apply,
                       .range = range,
                       .choices = {},
                       .fallback = fallback};
  };
  return {
      {.key = "tts.speed",
       .group = "voice",
       .type = SettingType::Decimal,
       .level = SettingLevel::Basic,
       .apply = SettingApply::Live,
       .range = {.min = 0.7, .max = 2.0, .step = 0.05},
       .choices = {},
       .fallback = "1"},
      {.key = "tts.quality",
       .group = "voice",
       .type = SettingType::Choice,
       .level = SettingLevel::Basic,
       .apply = SettingApply::Live,
       .range = {},
       .choices = {"auto", "low", "medium", "high"},
       .fallback = "auto"},
      integer("tts.steps_low", SettingLevel::Advanced, SettingApply::Live, {.min = 1, .max = 32, .step = 1}, "6"),
      integer("tts.steps_medium", SettingLevel::Advanced, SettingApply::Live, {.min = 1, .max = 32, .step = 1}, "8"),
      integer("tts.steps_high", SettingLevel::Advanced, SettingApply::Live, {.min = 1, .max = 32, .step = 1}, "12"),
      integer("tts.steps_cap", SettingLevel::Advanced, SettingApply::Live, {.min = 0, .max = 32, .step = 1}, "0"),
      integer("tts.max_chunk_len", SettingLevel::Advanced, SettingApply::Live, {.min = 30, .max = 2000, .step = 10}, "350"),
      integer("tts.edge_silence_ms", SettingLevel::Advanced, SettingApply::Restart, {.min = 0, .max = 1000, .step = 10}, "40"),
      integer("tts.join_silence_ms", SettingLevel::Advanced, SettingApply::Restart, {.min = 0, .max = 1000, .step = 10}, "90"),
      integer("tts.threads", SettingLevel::Advanced, SettingApply::Restart, {.min = 0, .max = 64, .step = 1}, "0"),
  };
}
