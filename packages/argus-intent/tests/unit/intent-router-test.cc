#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/intent/fasttext-classifier.hxx>
#include <shared/services/intent/intent-router.hxx>
#include <shared/services/memory/phrase-catalog.hxx>

#include <string>
#include <utility>
#include <vector>

namespace
{

// Neither a trigger, nor a statement-start opener in the catalog, nor a
// recall marker: only the model could decide this turn.
constexpr const char* kModelOnlyTurn = "el sofa del salon es nuevo";

class NullClassifier final : public intent::IIntentClassifier
{
public:
  bool isLoaded() const override { return false; }
  std::vector<intent::IntentHit> score(const std::string&) const override
  {
    return {};
  }
};

// Answers Camera with a perfect score no matter what, so only the rule layer
// can beat it.
class AlwaysCameraClassifier final : public intent::IIntentClassifier
{
public:
  bool isLoaded() const override { return true; }
  std::vector<intent::IntentHit> score(const std::string&) const override
  {
    return {{.intent = intent::ToolIntent::Camera, .score = 1.0F},
            {.intent = intent::ToolIntent::None, .score = 0.0F}};
  }
};

// Scripted hits, so the threshold and margin logic is testable without a
// model file on disk.
class ScriptedClassifier final : public intent::IIntentClassifier
{
public:
  explicit ScriptedClassifier(std::vector<intent::IntentHit> hits)
      : hits_(std::move(hits))
  {
  }

  bool isLoaded() const override { return true; }
  std::vector<intent::IntentHit> score(const std::string&) const override
  {
    return hits_;
  }

private:
  std::vector<intent::IntentHit> hits_;
};

// PhraseCatalog holds a mutex, so it is neither copyable nor movable: each
// test builds its own in place.
struct BuiltCatalog
{
  BuiltCatalog() { value.build(); }
  PhraseCatalog value;
};

} // namespace

TEST_CASE("router degrades to not-confident when the model is absent")
{
  BuiltCatalog catalog;
  const NullClassifier absent;
  const IntentRouter router(
      {.catalog = catalog.value, .model = absent, .recurrent = nullptr});

  const auto decision = router.decide(kModelOnlyTurn, "es");
  CHECK(decision.confident == false);
  CHECK(decision.intent == intent::ToolIntent::Unknown);
}

TEST_CASE("an explicit trigger is decided by the rules, not the model")
{
  BuiltCatalog catalog;
  const AlwaysCameraClassifier wrong;
  const IntentRouter router(
      {.catalog = catalog.value, .model = wrong, .recurrent = nullptr});

  const auto decision =
      router.decide("recuerda que mi hermana viene los domingos", "es");
  CHECK(decision.fromRules == true);
  CHECK(decision.confident == true);
  CHECK(decision.intent == intent::ToolIntent::MemorySave);
}

TEST_CASE("a recurring time stays a fact, a single instant becomes a reminder")
{
  BuiltCatalog catalog;
  const AlwaysCameraClassifier wrong;

  const IntentRouter recurring(
      {.catalog = catalog.value,
       .model = wrong,
       .recurrent = [](const std::string&, const std::string&) { return true; }});
  CHECK(recurring.decide("recuerda que el perro come a las 7", "es").intent ==
        intent::ToolIntent::MemorySave);

  const IntentRouter once(
      {.catalog = catalog.value,
       .model = wrong,
       .recurrent = [](const std::string&, const std::string&) { return false; }});
  CHECK(once.decide("recuerdame que la reunion es a las 3", "es").intent ==
        intent::ToolIntent::ReminderSet);
}

TEST_CASE("a confident score passes the threshold and the margin")
{
  BuiltCatalog catalog;
  const ScriptedClassifier confident(
      {{.intent = intent::ToolIntent::MemoryRecall, .score = 0.95F},
       {.intent = intent::ToolIntent::None, .score = 0.03F}});
  const IntentRouter router(
      {.catalog = catalog.value, .model = confident, .recurrent = nullptr});

  const auto decision = router.decide("donde puse las llaves de casa", "es");
  CHECK(decision.confident == true);
  CHECK(decision.fromRules == false);
  CHECK(decision.intent == intent::ToolIntent::MemoryRecall);
}

TEST_CASE("a thin margin hands the turn back to the LLM tier")
{
  BuiltCatalog catalog;
  const ScriptedClassifier uncertain(
      {{.intent = intent::ToolIntent::MemorySave, .score = 0.93F},
       {.intent = intent::ToolIntent::ReminderSet, .score = 0.85F}});
  const IntentRouter router(
      {.catalog = catalog.value, .model = uncertain, .recurrent = nullptr});

  CHECK(router.decide("la reunion del jueves a las tres", "es").confident ==
        false);
}

TEST_CASE("a cancellation is a forget decided by the rules")
{
  BuiltCatalog catalog;
  const AlwaysCameraClassifier wrong;
  const IntentRouter router(
      {.catalog = catalog.value, .model = wrong, .recurrent = nullptr});

  CHECK(router.decide("olvida lo que te dije del perro", "es").intent ==
        intent::ToolIntent::MemoryForget);
}

TEST_CASE("normalizeInput reproduces the training normalisation")
{
  CHECK(intent::normalizeInput("¿Qué estás viendo?") == "que estas viendo");
  CHECK(intent::normalizeInput("¡Hola, Mundo!") == "hola mundo");
  CHECK(intent::normalizeInput("Remember the MILK") == "remember the milk");
  CHECK(intent::normalizeInput("el sofá del salón es nuevo") == kModelOnlyTurn);
  CHECK(intent::normalizeInput("  Recuerda...   la   leche  ") ==
        "recuerda la leche");
  CHECK(intent::normalizeInput("Año: 2024") == "ano 2024");
}

TEST_CASE("the label round trip covers every class")
{
  for (const auto& [name, value] : intent::kIntentNames) {
    CHECK(intent::toolIntentFromString(name) == value);
    CHECK(intent::toolIntentToString(value) == name);
  }
  CHECK(intent::toolIntentFromString("not_a_class") ==
        intent::ToolIntent::Unknown);
}

// Argmax lands on the right class; whether it clears the 0.90 threshold is
// the accuracy gate's business, not this sentence's.
TEST_CASE("the real model, when published, decides a turn no rule covers")
{
  BuiltCatalog catalog;
  const FastTextClassifier model(
      std::string(ARGUS_TEST_INTENT_MODELS_DIR) + "/intent.bin");
  if (!model.isLoaded())
    return;

  const IntentRouter router(
      {.catalog = catalog.value, .model = model, .recurrent = nullptr});
  const auto decision = router.decide(kModelOnlyTurn, "es");
  CHECK(decision.fromRules == false);
  CHECK(decision.intent == intent::ToolIntent::MemorySave);
}
