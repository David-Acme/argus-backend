#include "vlm-settings.hxx"

namespace
{

struct IntegerSetting
{
  const char* key;
  const char* group;
  SettingLevel level{SettingLevel::Advanced};
  SettingApply apply{SettingApply::Restart};
  SettingRange range{};
  const char* fallback;
};

SettingSpec integer(const IntegerSetting& setting)
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

}

std::vector<SettingSpec> vlmSettingsCatalog()
{
  return {
      integer({.key = "vision.max_input_px",
               .group = "descriptions",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Live,
               .range = {.min = 128, .max = 1024, .step = 32},
               .fallback = "384"}),
      integer({.key = "vision.max_tokens",
               .group = "descriptions",
               .level = SettingLevel::Basic,
               .apply = SettingApply::Live,
               .range = {.min = 8, .max = 512, .step = 8},
               .fallback = "64"}),
      {.key = "vision.prompt",
       .group = "descriptions",
       .type = SettingType::Text,
       .level = SettingLevel::Advanced,
       .apply = SettingApply::Live,
       .range = {},
       .choices = {},
       .fallback = "Can you describe this image?"},
      integer({.key = "vision.caption_cache_slots",
               .group = "descriptions",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Live,
               .range = {.min = 1, .max = 64, .step = 1},
               .fallback = "8"}),
      integer({.key = "vision.image_max_tokens",
               .group = "engine",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 0, .max = 4096, .step = 16},
               .fallback = "0"}),
      integer({.key = "vision.context_size",
               .group = "engine",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 2048, .max = 32768, .step = 1024},
               .fallback = "8192"}),
      integer({.key = "vision.threads",
               .group = "engine",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = 0, .max = 64, .step = 1},
               .fallback = "0"}),
      integer({.key = "vision.gpu_layers",
               .group = "engine",
               .level = SettingLevel::Advanced,
               .apply = SettingApply::Restart,
               .range = {.min = -1, .max = 999, .step = 1},
               .fallback = "-1"}),
  };
}
