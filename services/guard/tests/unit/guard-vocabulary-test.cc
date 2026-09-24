#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/guard/vocabulary/decision-suppression.hxx>
#include <feature/guard/vocabulary/encounter-state.hxx>
#include <feature/guard/vocabulary/guard-action-kind.hxx>
#include <feature/guard/vocabulary/guard-danger.hxx>
#include <feature/guard/vocabulary/guard-intent-status.hxx>
#include <feature/guard/vocabulary/guard-mode.hxx>
#include <feature/guard/vocabulary/observation-status.hxx>

#include <cstddef>
#include <string>
#include <vector>

template <typename Enum>
struct CheckRoundTripInput
{
    const std::vector<Enum>& values;
    const std::vector<std::string>& names;
    std::string (*toString)(Enum);
    Enum (*fromString)(const std::string&);
};

template <typename Enum>
static void checkRoundTrip(const CheckRoundTripInput<Enum>& input)
{
    const auto& values = input.values;
    const auto& names = input.names;
    auto toString = input.toString;
    auto fromString = input.fromString;

    REQUIRE(values.size() == names.size());
    for (size_t i = 0; i < values.size(); ++i) {
        CHECK(toString(values[i]) == names[i]);
        CHECK(fromString(names[i]) == values[i]);
    }
}

TEST_CASE("guard intent status strings round-trip without a silent default")
{
    const std::vector<GuardIntentStatus> values = {
        GuardIntentStatus::Pending,      GuardIntentStatus::InFlight,
        GuardIntentStatus::RetryableFailed, GuardIntentStatus::Succeeded,
        GuardIntentStatus::DuplicateSucceeded, GuardIntentStatus::Rejected,
        GuardIntentStatus::Conflict,     GuardIntentStatus::Indeterminate};
    const std::vector<std::string> names = {
        "pending",      "in_flight",     "retryable_failed", "succeeded",
        "duplicate_succeeded", "rejected", "conflict",       "indeterminate"};
    REQUIRE(values.size() == names.size());
    for (size_t i = 0; i < values.size(); ++i) {
        CHECK(guardIntentStatusToString(values[i]) == names[i]);
        CHECK(guardIntentStatusFromString(names[i]) == values[i]);
    }
    CHECK_FALSE(guardIntentStatusFromString("bogus").has_value());
    CHECK_FALSE(guardIntentStatusFromString("").has_value());
    CHECK_FALSE(guardIntentStatusFromString("unknown").has_value());
}
