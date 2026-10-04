#include "profile-planner.hxx"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ranges>
#include <utility>

namespace
{
template <typename Number> std::optional<Number> parsed(const std::string& text)
{
  Number value{};
  const auto* last = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), last, value);
  if (text.empty() || error != std::errc{} || end != last)
    return std::nullopt;
  return value;
}

template <typename Number> bool sameNumber(const SettingEntry& entry, const std::string& target)
{
  const auto left = parsed<Number>(entry.value);
  const auto right = parsed<Number>(target);
  if (!left || !right)
    return entry.value == target;
  return *left == *right;
}

const OwnerCatalog* reachableCatalog(const std::vector<OwnerCatalog>& catalogs, const std::string& owner)
{
  const auto match = std::ranges::find(catalogs, owner, &OwnerCatalog::service);
  return match == catalogs.end() || !match->reachable ? nullptr : &*match;
}

const SettingEntry* entryOf(const OwnerCatalog& catalog, const std::string& key)
{
  const auto match = std::ranges::find(catalog.settings, key, [](const SettingEntry& entry) { return entry.spec.key; });
  return match == catalog.settings.end() ? nullptr : &*match;
}

std::optional<ChoiceState> installOf(const SettingEntry& entry, const std::string& target)
{
  if (entry.spec.type != SettingType::Choice)
    return std::nullopt;
  const auto state = std::ranges::find(entry.choiceStates, target, &ChoiceState::choice);
  if (state == entry.choiceStates.end() || state->availability == ChoiceAvailability::Installed)
    return std::nullopt;
  return *state;
}

std::size_t positionOf(const OwnerCatalog& catalog, const std::string& key)
{
  const auto match = std::ranges::find(catalog.settings, key, [](const SettingEntry& entry) { return entry.spec.key; });
  return static_cast<std::size_t>(std::distance(catalog.settings.begin(), match));
}

std::vector<SettingChange> inCatalogOrder(const ProfileOwnerChanges& owner, const OwnerCatalog* catalog)
{
  auto changes = settings_profile::effectiveChanges(owner, catalog);
  if (catalog != nullptr)
    std::ranges::stable_sort(changes, {}, [catalog](const SettingChange& change) { return positionOf(*catalog, change.key); });
  return changes;
}

OwnerPreview previewOwner(const ProfileOwnerChanges& owner, const OwnerCatalog* catalog)
{
  OwnerPreview preview{.service = owner.owner, .reachable = catalog != nullptr, .changes = {}};
  for (auto& change : inCatalogOrder(owner, catalog)) {
    if (catalog == nullptr) {
      preview.changes.push_back(
          {.key = std::move(change.key), .from = std::nullopt, .to = std::move(change.value), .apply = std::nullopt,
           .install = std::nullopt, .changed = true});
      continue;
    }
    const auto* entry = entryOf(*catalog, change.key);
    if (entry == nullptr)
      continue;
    preview.changes.push_back({.key = std::move(change.key),
                               .from = entry->value,
                               .to = change.value,
                               .apply = entry->spec.apply,
                               .install = installOf(*entry, change.value),
                               .changed = !settings_profile::sameValue(*entry, change.value)});
  }
  return preview;
}

bool hostOnly(const std::optional<ChoiceState>& install)
{
  return install && install->availability == ChoiceAvailability::HostOnly;
}

PlannedKey planKey(const SettingChange& change, const OwnerCatalog& catalog)
{
  const auto* entry = entryOf(catalog, change.key);
  if (entry == nullptr)
    return {.result = {.key = change.key,
                       .from = std::nullopt,
                       .to = change.value,
                       .status = ProfileKeyStatus::Rejected,
                       .reason = SettingRejectionReason::Unknown},
            .send = false};
  ProfileKeyResult result{
      .key = change.key, .from = entry->value, .to = change.value, .status = ProfileKeyStatus::Applied, .reason = {}};
  if (settings_profile::sameValue(*entry, change.value)) {
    result.status = ProfileKeyStatus::Unchanged;
    return {.result = std::move(result), .send = false};
  }
  if (hostOnly(installOf(*entry, change.value))) {
    result.status = ProfileKeyStatus::Rejected;
    result.reason = SettingRejectionReason::NotInstalled;
    return {.result = std::move(result), .send = false};
  }
  return {.result = std::move(result), .send = true};
}

OwnerPlan planOwner(const ProfileOwnerChanges& owner, const OwnerCatalog* catalog)
{
  OwnerPlan plan{.service = owner.owner,
                 .reachable = catalog != nullptr,
                 .keys = {},
                 .catalog = {},
                 .marker = std::nullopt,
                 .markerRecorded = false};
  if (catalog != nullptr)
    plan.catalog = *catalog;
  for (const auto& change : inCatalogOrder(owner, catalog)) {
    if (catalog == nullptr)
      plan.keys.push_back({.result = {.key = change.key,
                                      .from = std::nullopt,
                                      .to = change.value,
                                      .status = ProfileKeyStatus::Unreachable,
                                      .reason = std::nullopt},
                           .send = false});
    else
      plan.keys.push_back(planKey(change, *catalog));
  }
  return plan;
}

std::optional<RecommendationReason> shortfall(const RecommendationRule& rule, const HardwareFacts& hardware)
{
  if (hardware.cores < rule.minCores)
    return RecommendationReason::Cores;
  if (hardware.ramGb < rule.minRamGb)
    return RecommendationReason::Ram;
  if (rule.vectorIsa && !hardware.vectorIsa())
    return RecommendationReason::Isa;
  return std::nullopt;
}

PlannedKey sendKey(const SettingEntry& entry, const std::string& target)
{
  return {.result = {.key = entry.spec.key,
                     .from = entry.value,
                     .to = target,
                     .status = ProfileKeyStatus::Applied,
                     .reason = std::nullopt},
          .send = true};
}

const ProfileOwnerChanges* ownerOf(const SettingsProfile& profile, const std::string& service)
{
  const auto match = std::ranges::find(profile.owners, service, &ProfileOwnerChanges::owner);
  return match == profile.owners.end() ? nullptr : &*match;
}

OwnerPlan emptyPlan(const OwnerCatalog& catalog)
{
  return {.service = catalog.service,
          .reachable = catalog.reachable,
          .keys = {},
          .catalog = catalog,
          .marker = std::nullopt,
          .markerRecorded = false};
}

std::vector<std::string> keysWithStatus(const OwnerPlan& plan, ProfileKeyStatus status)
{
  std::vector<std::string> keys;
  for (const auto& planned : plan.keys)
    if (planned.result.status == status)
      keys.push_back(planned.result.key);
  return keys;
}

std::size_t pendingCount(const OwnerPlan& plan)
{
  return static_cast<std::size_t>(std::ranges::count_if(plan.keys, &PlannedKey::send));
}

struct Settlement
{
  ProfileKeyStatus status{ProfileKeyStatus::Rejected};
  std::optional<SettingRejectionReason> reason;
};

void settle(PlannedKey& planned, const Settlement& settlement)
{
  planned.send = false;
  planned.result.status = settlement.status;
  planned.result.reason = settlement.reason;
}

PlannedKey* pendingKey(OwnerPlan& plan, const std::string& key)
{
  const auto match = std::ranges::find_if(plan.keys, [&key](const PlannedKey& planned) {
    return planned.send && planned.result.key == key;
  });
  return match == plan.keys.end() ? nullptr : &*match;
}

void settleRemaining(OwnerPlan& plan, const Settlement& settlement)
{
  for (auto& planned : plan.keys | std::views::filter(&PlannedKey::send))
    settle(planned, settlement);
}
}

namespace settings_profile
{
std::vector<SettingChange> effectiveChanges(const ProfileOwnerChanges& owner, const OwnerCatalog* catalog)
{
  auto changes = owner.changes;
  if (catalog == nullptr)
    return changes;
  for (const auto& conditional : owner.withCapability)
    if (catalog->can(conditional.capability))
      changes.push_back(conditional.change);
  return changes;
}

bool sameValue(const SettingEntry& entry, const std::string& target)
{
  switch (entry.spec.type) {
  case SettingType::Integer: return sameNumber<long long>(entry, target);
  case SettingType::Decimal: return sameNumber<double>(entry, target);
  case SettingType::Toggle:
  case SettingType::Choice:
  case SettingType::Text: return entry.value == target;
  }
  return entry.value == target;
}

ProfilePreview preview(const SettingsProfile& profile, const std::vector<OwnerCatalog>& catalogs)
{
  ProfilePreview result{.id = profile.id, .labelKey = profile.labelKey, .current = true, .owners = {}};
  for (const auto& owner : profile.owners) {
    const auto* catalog = reachableCatalog(catalogs, owner.owner);
    if (settings_profile::effectiveChanges(owner, catalog).empty())
      continue;
    auto ownerPreview = previewOwner(owner, catalog);
    const bool settled = ownerPreview.reachable &&
                         ownerPreview.changes.size() == settings_profile::effectiveChanges(owner, catalog).size() &&
                         std::ranges::none_of(ownerPreview.changes, &ProfileChange::changed);
    result.current = result.current && settled;
    result.owners.push_back(std::move(ownerPreview));
  }
  return result;
}

std::vector<OwnerPlan> plan(const SettingsProfile& profile, const std::vector<OwnerCatalog>& catalogs)
{
  std::vector<OwnerPlan> plans;
  plans.reserve(profile.owners.size());
  for (const auto& owner : profile.owners) {
    const auto* catalog = reachableCatalog(catalogs, owner.owner);
    if (!settings_profile::effectiveChanges(owner, catalog).empty())
      plans.push_back(planOwner(owner, catalog));
  }
  return plans;
}

Recommendation recommend(const ProfileCatalog& catalog, const HardwareFacts& hardware)
{
  const auto& rules = catalog.rules;
  Recommendation recommendation{.profile = catalog.fallback,
                                .reason = RecommendationReason::Meets,
                                .hardware = hardware,
                                .rule = std::nullopt,
                                .missed = std::nullopt,
                                .rules = rules,
                                .fallback = catalog.fallback};
  for (const auto index : std::views::iota(std::size_t{0}, rules.size())) {
    if (shortfall(rules[index], hardware))
      continue;
    recommendation.profile = rules[index].profile;
    recommendation.rule = rules[index];
    if (index > 0) {
      recommendation.missed = rules[index - 1];
      recommendation.reason = shortfall(rules[index - 1], hardware).value_or(RecommendationReason::Meets);
    }
    return recommendation;
  }
  if (!rules.empty()) {
    recommendation.missed = rules.back();
    recommendation.reason = shortfall(rules.back(), hardware).value_or(RecommendationReason::Meets);
  }
  return recommendation;
}
}

namespace settings_profile
{
OwnerPlan firstRunPlan(const SettingsProfile& profile, const OwnerCatalog& catalog)
{
  auto plan = emptyPlan(catalog);
  const auto* owner = ownerOf(profile, catalog.service);
  if (owner == nullptr || !catalog.reachable)
    return plan;
  for (const auto& change : inCatalogOrder(*owner, &catalog)) {
    const auto* entry = entryOf(catalog, change.key);
    if (entry == nullptr || !sameValue(*entry, entry->spec.fallback) || sameValue(*entry, change.value) ||
        installOf(*entry, change.value))
      continue;
    plan.keys.push_back(sendKey(*entry, change.value));
  }
  return plan;
}

OwnerPlan revertPlan(const ProfileCatalog& profiles, const OwnerCatalog& catalog)
{
  auto plan = emptyPlan(catalog);
  if (!catalog.reachable || !catalog.profile || catalog.profile->origin != ProfileOrigin::Recommended)
    return plan;
  const auto* profile = profiles.find(catalog.profile->id);
  const auto* owner = profile == nullptr ? nullptr : ownerOf(*profile, catalog.service);
  if (owner == nullptr)
    return plan;
  const auto targets = effectiveChanges(*owner, &catalog);
  for (const auto& key : catalog.profile->keys) {
    const auto* entry = entryOf(catalog, key);
    const auto target = std::ranges::find(targets, key, &SettingChange::key);
    if (entry == nullptr || target == targets.end() || !sameValue(*entry, target->value) ||
        sameValue(*entry, entry->spec.fallback))
      continue;
    plan.keys.push_back(sendKey(*entry, entry->spec.fallback));
  }
  return plan;
}

std::optional<FirstRunState> firstRunState(const std::vector<OwnerCatalog>& catalogs)
{
  std::optional<FirstRunState> state;
  for (const auto& catalog : catalogs) {
    if (!catalog.profile || (catalog.profile->origin != ProfileOrigin::Recommended &&
                             catalog.profile->origin != ProfileOrigin::Reverted))
      continue;
    if (!state)
      state = FirstRunState{.profile = catalog.profile->id,
                            .origin = catalog.profile->origin,
                            .appliedAt = catalog.profile->appliedAt,
                            .owners = {}};
    if (catalog.profile->origin == ProfileOrigin::Recommended) {
      state->origin = ProfileOrigin::Recommended;
      state->profile = catalog.profile->id;
    }
    state->appliedAt = std::max(state->appliedAt, catalog.profile->appliedAt);
    state->owners.push_back({.service = catalog.service, .keys = catalog.profile->keys});
  }
  return state;
}
}

ProfileApplication::ProfileApplication(std::vector<OwnerPlan> plans) : plans_(std::move(plans)) {}

std::vector<OwnerWrite> ProfileApplication::pendingWrites() const
{
  std::vector<OwnerWrite> writes;
  for (const auto& plan : plans_) {
    if (!plan.reachable || pendingCount(plan) == 0)
      continue;
    OwnerWrite write{.owner = plan.service, .changes = {}, .marker = plan.marker};
    for (const auto& planned : plan.keys | std::views::filter(&PlannedKey::send))
      write.changes.push_back({.key = planned.result.key, .value = planned.result.to});
    if (write.marker)
      write.marker->keys = keysWithStatus(plan, ProfileKeyStatus::Applied);
    writes.push_back(std::move(write));
  }
  return writes;
}

std::vector<OwnerWrite> ProfileApplication::markerWrites() const
{
  std::vector<OwnerWrite> writes;
  for (const auto& plan : plans_) {
    if (!plan.reachable || !plan.marker || plan.markerRecorded)
      continue;
    OwnerWrite write{.owner = plan.service, .changes = {}, .marker = plan.marker};
    write.marker->keys = keysWithStatus(plan, ProfileKeyStatus::Applied);
    writes.push_back(std::move(write));
  }
  return writes;
}

void ProfileApplication::record(const std::vector<OwnerWriteResult>& results)
{
  for (const auto& result : results) {
    const auto plan = std::ranges::find(plans_, result.owner, &OwnerPlan::service);
    if (plan == plans_.end())
      continue;
    if (!result.reachable) {
      plan->reachable = false;
      settleRemaining(*plan, {.status = ProfileKeyStatus::Unreachable, .reason = std::nullopt});
      continue;
    }
    if (result.catalog)
      plan->catalog = result.catalog;
    plan->markerRecorded = plan->markerRecorded || result.markerRecorded;
    const auto before = pendingCount(*plan);
    for (const auto& rejection : result.rejected)
      if (auto* planned = pendingKey(*plan, rejection.key))
        settle(*planned, {.status = ProfileKeyStatus::Rejected, .reason = rejection.reason});
    for (const auto& key : result.applied)
      if (auto* planned = pendingKey(*plan, key))
        settle(*planned, {.status = ProfileKeyStatus::Applied, .reason = std::nullopt});
    const bool validationRefused = result.applied.empty() && !result.rejected.empty();
    if (!validationRefused || pendingCount(*plan) == before)
      settleRemaining(*plan, {.status = ProfileKeyStatus::Rejected, .reason = SettingRejectionReason::Invalid});
  }
}

std::vector<OwnerApplyResult> ProfileApplication::results() const
{
  std::vector<OwnerApplyResult> owners;
  owners.reserve(plans_.size());
  for (const auto& plan : plans_) {
    OwnerApplyResult owner{.service = plan.service, .reachable = plan.reachable, .results = {}, .catalog = {}};
    owner.results.reserve(plan.keys.size());
    for (const auto& planned : plan.keys)
      owner.results.push_back(planned.result);
    if (plan.reachable)
      owner.catalog = plan.catalog;
    owners.push_back(std::move(owner));
  }
  return owners;
}
