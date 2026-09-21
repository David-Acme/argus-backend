#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <phrase/lexicon-kind.hxx>
#include <phrase/memory-type.hxx>
#include <phrase/details/phrase-kind.hxx>

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

TEST_CASE("memory type strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {MemoryType::Persona, MemoryType::Episodic, MemoryType::Instruction,
                   MemoryType::System},
        .names = {"persona", "episodic", "instruction", "system"},
        .toString = memoryTypeToString,
        .fromString = memoryTypeFromString});
}

TEST_CASE("phrase kind strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {PhraseKind::Trigger, PhraseKind::Confirmation,
                   PhraseKind::StatementStart, PhraseKind::RecallMarker,
                   PhraseKind::Interrogative, PhraseKind::Filler,
                   PhraseKind::Cancellation},
        .names = {"trigger", "confirmation", "statement_start", "recall_marker",
                  "interrogative", "filler", "cancellation"},
        .toString = phraseKindToString,
        .fromString = phraseKindFromString});
}

TEST_CASE("lexicon kind strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {LexiconKind::Predicate, LexiconKind::Kinship,
                   LexiconKind::FirstPerson, LexiconKind::Stopword},
        .names = {"predicate", "kinship", "first_person", "stopword"},
        .toString = lexiconKindToString,
        .fromString = lexiconKindFromString});
}

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(memoryTypeFromString("bogus") == MemoryType::Persona);
    CHECK(phraseKindFromString("bogus") == PhraseKind::Trigger);
    CHECK(lexiconKindFromString("bogus") == LexiconKind::Predicate);
}
