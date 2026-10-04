#include "profile-file.hxx"

#include <config/settings-config.hxx>
#include <json/reader.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace
{
constexpr std::size_t kMaxIdLength = 32;
constexpr std::size_t kMaxLabelLength = 64;
constexpr std::size_t kMaxChangesPerOwner = 64;
constexpr std::size_t kMaxKeyLength = 128;
constexpr std::size_t kMaxValueLength = 512;
constexpr int kMaxCores = 1024;
constexpr double kMaxRamGb = 65536;

class ProfileFileError : public std::invalid_argument
{
public:
  using std::invalid_argument::invalid_argument;
};

struct Node
{
  const Json::Value& value;
  std::string where;
};

[[noreturn]] void reject(const std::string& problem)
{
  throw ProfileFileError(problem);
}

Node child(const Node& node, const std::string& name)
{
  return {.value = node.value[name], .where = node.where + "." + name};
}

const Json::Value& objectAt(const Node& node)
{
  if (!node.value.isObject() || node.value.empty())
    reject(node.where + ": must be a non-empty object");
  return node.value;
}

std::string text(const Node& node, std::size_t maxLength)
{
  if (!node.value.isString() || node.value.asString().empty() || node.value.asString().size() > maxLength)
    reject(node.where + ": must be a non-empty string of at most " + std::to_string(maxLength) + " characters");
  return node.value.asString();
}

std::string profileId(const Node& node)
{
  auto id = text(node, kMaxIdLength);
  const bool wellFormed = std::ranges::all_of(id, [](char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '-';
  });
  if (!wellFormed)
    reject(node.where + ": must hold only lowercase letters, digits and '-'");
  return id;
}

std::size_t ownerPosition(std::string_view owner)
{
  return static_cast<std::size_t>(
      std::distance(kSettingsOwnerOrder.begin(), std::ranges::find(kSettingsOwnerOrder, owner)));
}

ProfileOwnerChanges readOwner(const std::string& owner, const Node& node)
{
  if (ownerPosition(owner) == kSettingsOwnerOrder.size())
    reject(node.where + ": is not a settings owner");
  objectAt(node);
  if (node.value.size() > kMaxChangesPerOwner)
    reject(node.where + ": holds more than 64 settings");
  ProfileOwnerChanges changes{.owner = owner, .changes = {}};
  for (const auto& key : node.value.getMemberNames()) {
    if (key.empty() || key.size() > kMaxKeyLength)
      reject(node.where + ": has a key that is empty or longer than 128 characters");
    changes.changes.push_back({.key = key, .value = text(child(node, key), kMaxValueLength)});
  }
  return changes;
}

SettingsProfile readProfile(const Node& node)
{
  objectAt(node);
  SettingsProfile profile{.id = profileId(child(node, "id")),
                          .labelKey = text(child(node, "labelKey"), kMaxLabelLength),
                          .owners = {}};
  const auto owners = child(node, "owners");
  for (const auto& owner : objectAt(owners).getMemberNames())
    profile.owners.push_back(readOwner(owner, child(owners, owner)));
  std::ranges::sort(profile.owners, {}, [](const ProfileOwnerChanges& owner) { return ownerPosition(owner.owner); });
  return profile;
}

std::string knownProfile(const Node& node, const ProfileCatalog& catalog)
{
  auto id = profileId(node);
  if (catalog.find(id) == nullptr)
    reject(node.where + ": names no profile of this file");
  return id;
}

RecommendationRule readRule(const Node& node, const ProfileCatalog& catalog)
{
  objectAt(node);
  const auto cores = child(node, "minCores");
  if (!cores.value.isInt() || cores.value.asInt() < 0 || cores.value.asInt() > kMaxCores)
    reject(cores.where + ": must be an integer between 0 and 1024");
  const auto ram = child(node, "minRamGb");
  if (!ram.value.isNumeric() || ram.value.asDouble() < 0 || ram.value.asDouble() > kMaxRamGb)
    reject(ram.where + ": must be a number between 0 and 65536");
  const auto vector = child(node, "vectorIsa");
  if (!vector.value.isNull() && !vector.value.isBool())
    reject(vector.where + ": must be true or false");
  return {.profile = knownProfile(child(node, "profile"), catalog),
          .minCores = cores.value.asInt(),
          .minRamGb = ram.value.asDouble(),
          .vectorIsa = vector.value.isBool() && vector.value.asBool()};
}

Node element(const Node& list, Json::ArrayIndex index)
{
  return {.value = list.value[index], .where = list.where + "[" + std::to_string(index) + "]"};
}

ProfileCatalog readCatalog(const Json::Value& root)
{
  const Node file{.value = root, .where = "profile file"};
  objectAt(file);
  const auto profiles = child(file, "profiles");
  if (!profiles.value.isArray() || profiles.value.empty())
    reject(profiles.where + ": must be a non-empty array");
  ProfileCatalog catalog;
  for (Json::ArrayIndex index = 0; index < profiles.value.size(); ++index) {
    const auto node = element(profiles, index);
    auto profile = readProfile(node);
    if (catalog.find(profile.id) != nullptr)
      reject(node.where + ".id: repeats the id " + profile.id);
    catalog.profiles.push_back(std::move(profile));
  }
  const auto recommendation = child(file, "recommendation");
  objectAt(recommendation);
  const auto rules = child(recommendation, "rules");
  if (!rules.value.isArray())
    reject(rules.where + ": must be an array");
  for (Json::ArrayIndex index = 0; index < rules.value.size(); ++index)
    catalog.rules.push_back(readRule(element(rules, index), catalog));
  catalog.fallback = knownProfile(child(recommendation, "fallback"), catalog);
  return catalog;
}
}

ProfileFileParse parseProfileCatalog(const Json::Value& root)
{
  try {
    return {.catalog = readCatalog(root), .problem = {}};
  }
  catch (const ProfileFileError& error) {
    return {.catalog = std::nullopt, .problem = error.what()};
  }
}

std::optional<ProfileCatalog> loadProfileFile(const std::string& path)
{
  std::ifstream file(path);
  if (!file) {
    LOG_ERROR << "Settings profiles: " << path << " could not be opened; the profile routes answer 503";
    return std::nullopt;
  }
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  if (!Json::parseFromStream(builder, file, &root, &errors)) {
    LOG_ERROR << "Settings profiles: " << path << " is not valid JSON (" << errors << "); the profile routes answer 503";
    return std::nullopt;
  }
  auto parsed = parseProfileCatalog(root);
  if (!parsed.catalog) {
    LOG_ERROR << "Settings profiles: " << path << " " << parsed.problem << "; the profile routes answer 503";
    return std::nullopt;
  }
  LOG_INFO << "Settings profiles: " << parsed.catalog->profiles.size() << " loaded from " << path;
  return std::move(parsed.catalog);
}
