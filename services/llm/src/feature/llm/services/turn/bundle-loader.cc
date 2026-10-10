#include "bundle-loader.hxx"

#include <text/sha256.hxx>

#include <json/reader.h>
#include <json/value.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

namespace turn
{

namespace
{
constexpr std::size_t kHashBlock = 1U << 22U;

constexpr std::array<std::string_view, 8> kLayout{
    "model.onnx", "tokenizer", "labels.json", "decision.json", "max_len", "model-card.md", "sha256", "manifest.json"};

std::string lowered(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string trimmed(std::string_view text)
{
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

std::optional<std::string> readFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::optional<std::string> hashFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return std::nullopt;
  argus::hash::Sha256 hasher;
  std::array<char, kHashBlock> block{};
  while (in) {
    in.read(block.data(), static_cast<std::streamsize>(block.size()));
    const std::streamsize read = in.gcount();
    if (read > 0)
      hasher.update(block.data(), static_cast<std::size_t>(read));
  }
  return argus::hash::hex(hasher.digest());
}

std::optional<Json::Value> readJson(const std::filesystem::path& path)
{
  const std::optional<std::string> bytes = readFile(path);
  if (!bytes)
    return std::nullopt;
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::istringstream stream(*bytes);
  std::string errors;
  if (!Json::parseFromStream(builder, stream, &root, &errors))
    return std::nullopt;
  return root;
}

struct Digests
{
  std::map<std::string, std::string> byName;
  bool ok{false};
  std::string error;
};

Digests parseDigests(const std::string& text)
{
  Digests digests{.byName = {}, .ok = true, .error = {}};
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty())
      continue;
    const std::size_t split = line.find("  ");
    if (split == std::string::npos) {
      digests.ok = false;
      digests.error = "the sha256 file has a line that is not in sha256sum form";
      return digests;
    }
    const std::string digest = lowered(trimmed(line.substr(0, split)));
    const std::string name = trimmed(line.substr(split + 2));
    if (digest.size() != 64 || name.empty()) {
      digests.ok = false;
      digests.error = "the sha256 file has a line that is not in sha256sum form";
      return digests;
    }
    digests.byName[name] = digest;
  }
  return digests;
}

std::optional<std::map<std::string, std::string>> listBundleFiles(const std::filesystem::path& dir, std::string& error)
{
  std::map<std::string, std::string> files;
  std::error_code code;
  for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, code), end;
       it != end;
       it.increment(code)) {
    if (code) {
      error = "the bundle cannot be read: " + code.message();
      return std::nullopt;
    }
    if (!it->is_regular_file(code))
      continue;
    files[it->path().lexically_relative(dir).generic_string()] = it->path().string();
  }
  return files;
}

std::optional<std::vector<std::string>> readDeciderLabels(const Json::Value& root)
{
  const Json::Value& node = root["labels"];
  if (!node.isArray())
    return std::nullopt;
  std::vector<std::string> labels;
  for (const Json::Value& item : node) {
    if (!item.isString())
      return std::nullopt;
    labels.push_back(item.asString());
  }
  return labels;
}

std::optional<std::map<std::string, std::vector<std::string>>> readExtractorFields(const Json::Value& root)
{
  const Json::Value& node = root["types"];
  if (!node.isObject())
    return std::nullopt;
  std::map<std::string, std::vector<std::string>> fields;
  for (const std::string& type : node.getMemberNames()) {
    const Json::Value& order = node[type];
    if (!order.isArray())
      return std::nullopt;
    std::vector<std::string>& names = fields[type];
    for (const Json::Value& item : order) {
      if (!item.isString())
        return std::nullopt;
      names.push_back(item.asString());
    }
  }
  return fields;
}
}

BundleLoader::BundleLoader(BundleLocation location)
    : dir_(std::move(location.dir)), pin_(lowered(trimmed(location.pin))), kind_(location.kind)
{
  load();
}

void BundleLoader::load()
{
  std::error_code code;
  if (!std::filesystem::is_directory(dir_, code)) {
    error_ = "the bundle " + dir_.string() + " is not there";
    return;
  }
  const std::optional<std::string> listed = readFile(dir_ / "sha256");
  if (!listed) {
    error_ = dir_.string() + ": the bundle has no sha256 file";
    return;
  }
  if (argus::hash::sha256Hex(*listed) != pin_) {
    error_ = dir_.string() + ": the sha256 file does not hash to the configured pin " + pin_;
    return;
  }
  const Digests digests = parseDigests(*listed);
  if (!digests.ok) {
    error_ = dir_.string() + ": " + digests.error;
    return;
  }
  std::string listingError;
  const std::optional<std::map<std::string, std::string>> files = listBundleFiles(dir_, listingError);
  if (!files) {
    error_ = dir_.string() + ": " + listingError;
    return;
  }
  for (const auto& [name, digest] : digests.byName) {
    const auto found = files->find(name);
    if (found == files->end()) {
      error_ = dir_.string() + ": sha256 names " + name + ", which is not in the bundle";
      return;
    }
    const std::optional<std::string> actual = hashFile(found->second);
    if (!actual || *actual != digest) {
      error_ = dir_.string() + ": " + name + " does not match its line in sha256";
      return;
    }
  }
  for (const auto& [name, path] : *files) {
    if (name == "sha256" || digests.byName.contains(name))
      continue;
    error_ = dir_.string() + ": the bundle holds " + name + ", which sha256 does not list";
    return;
  }
  for (const std::string_view entry : kLayout) {
    if (!std::filesystem::exists(dir_ / std::string(entry), code)) {
      error_ = dir_.string() + ": the bundle is missing " + std::string(entry);
      return;
    }
  }
  const std::optional<Json::Value> manifest = readJson(dir_ / "manifest.json");
  if (!manifest) {
    error_ = dir_.string() + ": manifest.json cannot be read";
    return;
  }
  fitSplit_ = (*manifest)["calibration"].get("fitOn", "").asString();
  const std::optional<Json::Value> decision = readJson(dir_ / "decision.json");
  if (!decision) {
    error_ = dir_.string() + ": decision.json cannot be read";
    return;
  }
  if (fitSplit_.empty())
    fitSplit_ = (*decision)["source"].get("fitOn", "").asString();
  if ((*decision).isMember("act") || (*decision).isMember("ask")) {
    policy_.act = (*decision).get("act", 0.0).asDouble();
    policy_.ask = (*decision).get("ask", 0.0).asDouble();
    policy_.margin = (*decision).get("margin", 0.0).asDouble();
    policy_.now = (*decision).get("now", 0.0).asDouble();
    policy_.guardMemory = (*decision).get("guardMemory", false).asBool();
    policy_.present = true;
  }
  if (kind_ == BundleKind::Extractor && ((*decision).isMember("pairThreshold") || (*decision).isMember("maxSpanWords"))) {
    thresholds_.threshold = (*decision).get("pairThreshold", 0.0).asDouble();
    thresholds_.maxSpanWidth = (*decision).get("maxSpanWords", 0).asInt();
    thresholds_.present = true;
  }
  if (kind_ == BundleKind::Decider && ((*decision).isMember("threshold") || (*decision).isMember("maxSpanWidth"))) {
    thresholds_.threshold = (*decision).get("threshold", 0.0).asDouble();
    thresholds_.maxSpanWidth = (*decision).get("maxSpanWidth", 0).asInt();
    thresholds_.present = true;
  }
  if (const std::optional<CalibrationModel> confidence = calibrationFromJson((*decision)["calibration"]["confidence"]))
    confidenceCalibration_ = *confidence;
  if (const std::optional<CalibrationModel> now = calibrationFromJson((*decision)["calibration"]["now"]))
    nowCalibration_ = *now;
  const bool declaresHeadMaxLen = (*decision).isMember("head_max_len");
  const bool declaresTemperature = (*decision).isMember("temperature");
  if (kind_ == BundleKind::Decider && !(declaresHeadMaxLen && declaresTemperature)) {
    error_ = dir_.string() + ": a decider bundle's decision.json must declare both head_max_len and temperature, and this one " +
             (declaresHeadMaxLen || declaresTemperature ? "declares only one of them"
                                                        : "declares neither head_max_len nor temperature");
    return;
  }
  if (const Json::Value& headMaxLen = (*decision)["head_max_len"]; headMaxLen.isInt() && headMaxLen.asInt() > 0)
    decode_.headMaxLen = headMaxLen.asInt();
  if (const Json::Value& temperature = (*decision)["temperature"]; temperature.isArray() && temperature.size() == decode_.temperature.size()) {
    std::array<double, 3> values{};
    bool numeric = true;
    for (Json::ArrayIndex index = 0; index < values.size(); ++index) {
      numeric = numeric && temperature[index].isNumeric();
      values[index] = numeric ? temperature[index].asDouble() : 1.0;
    }
    if (numeric)
      decode_.temperature = values;
  }
  const std::optional<Json::Value> labels = readJson(dir_ / "labels.json");
  if (!labels) {
    error_ = dir_.string() + ": labels.json cannot be read";
    return;
  }
  if (kind_ == BundleKind::Extractor) {
    const std::optional<std::map<std::string, std::vector<std::string>>> types = readExtractorFields(*labels);
    if (!types) {
      error_ = dir_.string() + ": an extractor bundle's labels.json is the type-keyed object form, and this one is not";
      return;
    }
    if (types->empty()) {
      error_ = dir_.string() + ": labels.json names no type";
      return;
    }
    for (const auto& [type, order] : *types) {
      if (order.empty()) {
        error_ = dir_.string() + ": labels.json names the type '" + type + "' with no field";
        return;
      }
    }
    typeFields_ = *types;
  }
  else {
    const std::optional<std::vector<std::string>> names = readDeciderLabels(*labels);
    if (!names) {
      error_ = dir_.string() + ": a decider bundle's labels.json is the label array form, and this one is not";
      return;
    }
    labels_ = *names;
    if (labels_.empty()) {
      error_ = dir_.string() + ": labels.json names no label";
      return;
    }
  }
  const std::optional<std::string> maxLen = readFile(dir_ / "max_len");
  if (!maxLen) {
    error_ = dir_.string() + ": max_len cannot be read";
    return;
  }
  const std::string value = trimmed(*maxLen);
  if (value.empty() || !std::ranges::all_of(value, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
    error_ = dir_.string() + ": max_len is not a positive integer";
    return;
  }
  maxLen_ = std::stoi(value);
  if (maxLen_ <= 0) {
    error_ = dir_.string() + ": max_len is not a positive integer";
    return;
  }
  valid_ = true;
}

}
