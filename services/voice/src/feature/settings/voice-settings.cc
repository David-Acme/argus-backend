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

SettingSpec watchInteger(const NumericSetting& setting)
{
  SettingSpec spec = integer(setting);
  spec.apply = SettingApply::Restart;
  return spec;
}

SettingSpec watchDecimal(const NumericSetting& setting)
{
  SettingSpec spec = decimal(setting);
  spec.apply = SettingApply::Restart;
  return spec;
}

void appendWatch(std::vector<SettingSpec>& catalog)
{
  catalog.push_back({.key = "watch.enabled",
                     .group = "watchman",
                     .type = SettingType::Toggle,
                     .level = SettingLevel::Advanced,
                     .apply = SettingApply::Restart,
                     .range = {},
                     .choices = {},
                     .fallback = "true"});
  catalog.push_back({.key = "watch.stage1",
                     .group = "watchman",
                     .type = SettingType::Choice,
                     .level = SettingLevel::Advanced,
                     .apply = SettingApply::Restart,
                     .range = {},
                     .choices = {"auto", "kws", "stt"},
                     .fallback = "auto"});
  const std::vector<NumericSetting> integers{
      {.key = "watch.list_refresh_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1000, .max = 120000, .step = 1000}, .fallback = "10000"},
      {.key = "watch.reconnect_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 200, .max = 60000, .step = 100}, .fallback = "2000"},
      {.key = "watch.ring_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1000, .max = 5000, .step = 100}, .fallback = "2000"},
      {.key = "watch.post_roll_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 200, .max = 3000, .step = 100}, .fallback = "1000"},
      {.key = "watch.wake_max_distance", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1, .max = 3, .step = 1}, .fallback = "2"},
      {.key = "watch.stt_stage1_max_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 500, .max = 5000, .step = 100}, .fallback = "2500"},
      {.key = "watch.stt_stage1_min_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 100, .max = 2000, .step = 50}, .fallback = "300"},
      {.key = "watch.kws_hangover_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 2000, .step = 50}, .fallback = "400"},
      {.key = "watch.kws_retry_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1000, .max = 60000, .step = 1000}, .fallback = "5000"},
      {.key = "watch.fire_debounce_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 200, .max = 10000, .step = 100}, .fallback = "1500"},
      {.key = "watch.broadcast_window_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 50, .max = 1000, .step = 10}, .fallback = "150"},
      {.key = "watch.broadcast_min_cameras", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 2, .max = 16, .step = 1}, .fallback = "3"},
      {.key = "watch.echo_tail_ms", .group = "watchman", .level = SettingLevel::Basic,
       .range = {.min = 0, .max = 3000, .step = 50}, .fallback = "800"},
      {.key = "watch.echo_max_lag_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 100, .max = 3000, .step = 50}, .fallback = "1000"},
      {.key = "watch.fire_log_size", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 10, .max = 5000, .step = 10}, .fallback = "200"},
      {.key = "watch.arbitration_window_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 100, .max = 2000, .step = 50}, .fallback = "400"},
      {.key = "watch.stage2_candidates", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1, .max = 4, .step = 1}, .fallback = "2"},
      {.key = "watch.end_silence_ms", .group = "watchman", .level = SettingLevel::Basic,
       .range = {.min = 300, .max = 3000, .step = 50}, .fallback = "700"},
      {.key = "watch.incomplete_hold_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 300, .max = 5000, .step = 100}, .fallback = "2000"},
      {.key = "watch.max_turn_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 5000, .max = 120000, .step = 1000}, .fallback = "30000"},
      {.key = "watch.ack_after_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 300, .max = 10000, .step = 100}, .fallback = "1500"},
      {.key = "watch.fail_after_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1000, .max = 60000, .step = 500}, .fallback = "8000"},
      {.key = "watch.idle_close_ms", .group = "watchman", .level = SettingLevel::Basic,
       .range = {.min = 5000, .max = 300000, .step = 1000}, .fallback = "20000"},
      {.key = "watch.max_length_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 60000, .max = 3600000, .step = 60000}, .fallback = "600000"},
      {.key = "watch.resumable_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 10000, .max = 1800000, .step = 10000}, .fallback = "180000"},
      {.key = "watch.reply_gap_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 200, .max = 5000, .step = 100}, .fallback = "700"},
      {.key = "watch.camera_drop_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 500, .max = 30000, .step = 500}, .fallback = "3000"},
      {.key = "watch.face_window_ms", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 300, .max = 3000, .step = 100}, .fallback = "1500"},
      {.key = "watch.visitor_consent_asks", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1, .max = 5, .step = 1}, .fallback = "2"},
      {.key = "watch.visitor_consent_max_words", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 1, .max = 12, .step = 1}, .fallback = "6"},
  };
  const std::vector<NumericSetting> decimals{
      {.key = "watch.stt_stage1_min_snr_db", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 30, .step = 0.5}, .fallback = "6.0"},
      {.key = "watch.min_wake_snr_db", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 30, .step = 0.5}, .fallback = "3.0"},
      {.key = "watch.broadcast_spread_db", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 20, .step = 0.5}, .fallback = "6.0"},
      {.key = "watch.tv_raise_db", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 30, .step = 0.5}, .fallback = "6.0"},
      {.key = "watch.echo_correlation", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0.1, .max = 1.0, .step = 0.05}, .fallback = "0.6"},
      {.key = "watch.echo_raise_db", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 30, .step = 0.5}, .fallback = "6.0"},
      {.key = "watch.floor_rise_db_per_s", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0.05, .max = 10, .step = 0.05}, .fallback = "0.5"},
      {.key = "watch.floor_fall_db_per_s", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0.5, .max = 60, .step = 0.5}, .fallback = "10.0"},
      {.key = "watch.weight_wake", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 5, .step = 0.1}, .fallback = "1.0"},
      {.key = "watch.weight_snr", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 1, .step = 0.01}, .fallback = "0.1"},
      {.key = "watch.weight_stage2", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 5, .step = 0.1}, .fallback = "1.0"},
      {.key = "watch.gain_weight", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0, .max = 1, .step = 0.05}, .fallback = "0.5"},
      {.key = "watch.gain_alpha", .group = "watchman", .level = SettingLevel::Advanced,
       .range = {.min = 0.01, .max = 1, .step = 0.01}, .fallback = "0.1"},
  };
  for (const NumericSetting& setting : integers)
    catalog.push_back(watchInteger(setting));
  for (const NumericSetting& setting : decimals)
    catalog.push_back(watchDecimal(setting));
}

}

std::vector<SettingSpec> voiceSettingsCatalog()
{
  std::vector<SettingSpec> catalog{
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
               .fallback = "0.30"}),
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
               .fallback = "6"}),
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
  appendWatch(catalog);
  return catalog;
}
