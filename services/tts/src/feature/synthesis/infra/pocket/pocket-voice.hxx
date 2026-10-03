#pragma once

#include "pocket-bundle.hxx"

#include <cstdint>
#include <filesystem>
#include <vector>

struct PocketVoice
{
  std::int64_t length{0};
  std::vector<std::vector<float>> caches;
};

struct PocketVoiceFile
{
  std::filesystem::path path;
  const PocketBundle& bundle;
};

[[nodiscard]] PocketVoice loadPocketVoice(const PocketVoiceFile& file);

[[nodiscard]] std::size_t voiceCacheSlots(const PocketBundle& bundle);
