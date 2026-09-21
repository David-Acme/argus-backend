#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <productivity/membership-error.hxx>
#include <productivity/reminder-detail-status.hxx>
#include <productivity/share-access.hxx>

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

TEST_CASE("reminder detail status strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {ReminderDetailStatus::Pending, ReminderDetailStatus::InProgress,
                   ReminderDetailStatus::Done, ReminderDetailStatus::Blocked},
        .names = {"pending", "in_progress", "done", "blocked"},
        .toString = reminderDetailStatusToString,
        .fromString = reminderDetailStatusFromString});
}

TEST_CASE("share access strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {ShareAccess::View, ShareAccess::Edit},
        .names = {"view", "edit"},
        .toString = shareAccessToString,
        .fromString = shareAccessFromString});
}

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(reminderDetailStatusFromString("bogus") ==
          ReminderDetailStatus::Pending);
    CHECK(shareAccessFromString("bogus") == ShareAccess::View);
}
