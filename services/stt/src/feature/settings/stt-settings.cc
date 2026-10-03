#include "stt-settings.hxx"

std::vector<SettingSpec> sttSettingsCatalog()
{
  return {
      {.key = "stt.language",
       .group = "recognition",
       .type = SettingType::Choice,
       .level = SettingLevel::Advanced,
       .apply = SettingApply::Live,
       .range = {},
       .choices = {"es", "en", "auto"},
       .fallback = "es"},
      {.key = "stt.engine",
       .group = "recognition",
       .type = SettingType::Choice,
       .level = SettingLevel::Advanced,
       .apply = SettingApply::Restart,
       .range = {},
       .choices = {"nemo_transducer", "whisper", "canary", "nemo_ctc", "omnilingual"},
       .fallback = "nemo_transducer"},
  };
}
