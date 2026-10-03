#include "pocket-voice.hxx"

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
constexpr std::uint64_t kMaxHeaderBytes = std::uint64_t{16} * 1024 * 1024;

struct SafetensorsFile
{
  nlohmann::json header;
  std::vector<char> data;
};

SafetensorsFile readSafetensors(const std::filesystem::path& path)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Pocket voice not found: " + path.filename().string());
  std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (bytes.size() < sizeof(std::uint64_t))
    throw std::runtime_error("Pocket voice is truncated: " + path.filename().string());
  std::uint64_t headerBytes = 0;
  std::memcpy(&headerBytes, bytes.data(), sizeof(headerBytes));
  if (headerBytes > kMaxHeaderBytes || headerBytes + sizeof(std::uint64_t) > bytes.size())
    throw std::runtime_error("Pocket voice header is invalid: " + path.filename().string());
  const auto headerBegin = bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint64_t));
  const auto headerEnd = headerBegin + static_cast<std::ptrdiff_t>(headerBytes);
  SafetensorsFile file{.header = nlohmann::json::parse(headerBegin, headerEnd), .data = {}};
  file.data.assign(headerEnd, bytes.end());
  return file;
}

struct TensorView
{
  std::string dtype;
  std::vector<std::int64_t> shape;
  const char* bytes{nullptr};
  std::size_t size{0};
};

struct TensorLookup
{
  const SafetensorsFile& file;
  std::string name;
};

std::optional<TensorView> tensor(const TensorLookup& lookup)
{
  const auto found = lookup.file.header.find(lookup.name);
  if (found == lookup.file.header.end())
    return std::nullopt;
  const auto offsets = found->at("data_offsets").get<std::vector<std::size_t>>();
  if (offsets.size() != 2 || offsets[0] > offsets[1] || offsets[1] > lookup.file.data.size())
    throw std::runtime_error("Pocket voice tensor is out of bounds: " + lookup.name);
  return TensorView{.dtype = found->at("dtype").get<std::string>(),
                    .shape = found->at("shape").get<std::vector<std::int64_t>>(),
                    .bytes = lookup.file.data.data() + offsets[0],
                    .size = offsets[1] - offsets[0]};
}

std::int64_t voiceLength(const SafetensorsFile& file, const PocketStateSpec& spec)
{
  if (const auto offset = tensor({.file = file, .name = spec.module + "/offset"}); offset.has_value()) {
    if (offset->dtype != "I64" || offset->size != sizeof(std::int64_t))
      throw std::runtime_error("Pocket voice offset is invalid: " + spec.module);
    std::int64_t value = 0;
    std::memcpy(&value, offset->bytes, sizeof(value));
    return value;
  }
  if (const auto end = tensor({.file = file, .name = spec.module + "/current_end"}); end.has_value() && !end->shape.empty())
    return end->shape.front();
  throw std::runtime_error("Pocket voice has no offset for " + spec.module);
}
}

std::size_t voiceCacheSlots(const PocketBundle& bundle)
{
  std::size_t slots = 0;
  for (const auto& spec : bundle.flowStates) {
    if (spec.key == "cache")
      ++slots;
  }
  return slots;
}

PocketVoice loadPocketVoice(const PocketVoiceFile& file)
{
  const auto contents = readSafetensors(file.path);
  PocketVoice voice;
  voice.caches.reserve(voiceCacheSlots(file.bundle));
  std::optional<std::int64_t> length;
  for (const auto& spec : file.bundle.flowStates) {
    if (spec.key != "cache")
      continue;
    const auto cache = tensor({.file = contents, .name = spec.module + "/cache"});
    if (!cache.has_value() || cache->dtype != "F32" || cache->shape.size() != 5 || spec.shape.size() != 5)
      throw std::runtime_error("Pocket voice cache is missing or not float32: " + spec.module);
    const auto& shape = cache->shape;
    if (shape[0] != 2 || shape[1] != 1 || shape[3] != spec.shape[3] || shape[4] != spec.shape[4] ||
        shape[2] <= 0 || shape[2] > spec.shape[2])
      throw std::runtime_error("Pocket voice cache does not fit the model: " + spec.module);
    const auto positions = voiceLength(contents, spec);
    if (positions <= 0 || positions > shape[2])
      throw std::runtime_error("Pocket voice offset is out of range: " + spec.module);
    if (length.has_value() && *length != positions)
      throw std::runtime_error("Pocket voice layers disagree on their length");
    length = positions;
    const auto rowFloats = static_cast<std::size_t>(shape[3]) * static_cast<std::size_t>(shape[4]);
    const auto storedRows = static_cast<std::size_t>(shape[2]);
    const auto usedRows = static_cast<std::size_t>(positions);
    if (cache->size != 2 * storedRows * rowFloats * sizeof(float))
      throw std::runtime_error("Pocket voice cache size does not match its shape: " + spec.module);
    std::vector<float> values(2 * usedRows * rowFloats);
    for (std::size_t half = 0; half < 2; ++half) {
      const auto* source = cache->bytes + (half * storedRows * rowFloats * sizeof(float));
      std::memcpy(values.data() + (half * usedRows * rowFloats), source, usedRows * rowFloats * sizeof(float));
    }
    voice.caches.push_back(std::move(values));
  }
  if (!length.has_value())
    throw std::runtime_error("Pocket model has no attention cache");
  voice.length = *length;
  return voice;
}
