#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <voice-lang.hxx>

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

TEST_CASE("voice language strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {VoiceLang::Es, VoiceLang::En},
        .names = {"es", "en"},
        .toString = voiceLangToString,
        .fromString = voiceLangFromString});
    CHECK(voiceLangToString(VoiceLang::System) == "");
    CHECK(voiceLangFromString("") == VoiceLang::System);
}

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(voiceLangFromString("bogus") == VoiceLang::System);
}
