#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/intent/services/fasttext-classifier.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <phrase/phrase-catalog.hxx>

#include <string>
#include <utility>
#include <vector>

namespace
{

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

struct BuiltCatalog
{
  BuiltCatalog() { value.build(); }
  PhraseCatalog value;
};

}

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

TEST_CASE("a cancellation is a forget only when the model agrees")
{
  BuiltCatalog catalog;
  const ScriptedClassifier agrees(
      {{.intent = intent::ToolIntent::MemoryForget, .score = 0.62F},
       {.intent = intent::ToolIntent::None, .score = 0.30F}});
  const IntentRouter router(
      {.catalog = catalog.value, .model = agrees, .recurrent = nullptr});

  const auto decision = router.decide("olvida lo que te dije del perro", "es");
  CHECK(decision.confident);
  CHECK(decision.fromRules);
  CHECK(decision.source == intent::DecisionSource::Cancellation);
  CHECK(decision.intent == intent::ToolIntent::MemoryForget);
  CHECK(router.decide("forget that, it doesn't matter", "en").intent ==
        intent::ToolIntent::MemoryForget);
}

TEST_CASE("a cancellation the model does not back is left to the LLM tier")
{
  BuiltCatalog catalog;
  const ScriptedClassifier none(
      {{.intent = intent::ToolIntent::None, .score = 0.80F},
       {.intent = intent::ToolIntent::MemoryForget, .score = 0.10F}});
  const IntentRouter router(
      {.catalog = catalog.value, .model = none, .recurrent = nullptr});

  CHECK_FALSE(router.decide("olvidalo, no era importante", "es").confident);
  CHECK_FALSE(router.decide("never mind, forget it", "en").confident);

  const NullClassifier absent;
  const IntentRouter blind(
      {.catalog = catalog.value, .model = absent, .recurrent = nullptr});
  CHECK_FALSE(blind.decide("olvida lo que te dije del perro", "es").confident);
}

TEST_CASE("a statement or a recall marker needs the model to agree with it")
{
  BuiltCatalog catalog;
  const ScriptedClassifier saves(
      {{.intent = intent::ToolIntent::MemorySave, .score = 0.62F},
       {.intent = intent::ToolIntent::None, .score = 0.30F}});
  const IntentRouter agreeing(
      {.catalog = catalog.value, .model = saves, .recurrent = nullptr});
  const auto agreed = agreeing.decide("la lavadora nueva tiene dos anos de garantia", "es");
  CHECK(agreed.confident);
  CHECK(agreed.source == intent::DecisionSource::Statement);
  CHECK(agreed.intent == intent::ToolIntent::MemorySave);

  const ScriptedClassifier none(
      {{.intent = intent::ToolIntent::None, .score = 0.85F},
       {.intent = intent::ToolIntent::MemorySave, .score = 0.05F}});
  const IntentRouter silent(
      {.catalog = catalog.value, .model = none, .recurrent = nullptr});
  CHECK_FALSE(silent.decide("la lavadora nueva tiene dos anos de garantia", "es").confident);
  CHECK_FALSE(silent.decide("tell me a joke", "en").confident);

  const ScriptedClassifier weak(
      {{.intent = intent::ToolIntent::MemorySave, .score = 0.40F},
       {.intent = intent::ToolIntent::None, .score = 0.35F}});
  const IntentRouter hesitant(
      {.catalog = catalog.value, .model = weak, .recurrent = nullptr});
  CHECK_FALSE(hesitant.decide("la lavadora nueva tiene dos anos de garantia", "es").confident);
}

TEST_CASE("a confident model outranks a rule that proposes another class")
{
  BuiltCatalog catalog;
  const ScriptedClassifier reminder(
      {{.intent = intent::ToolIntent::ReminderSet, .score = 0.97F},
       {.intent = intent::ToolIntent::None, .score = 0.01F}});
  const IntentRouter router(
      {.catalog = catalog.value, .model = reminder, .recurrent = nullptr});

  const auto decision = router.decide("remind me to call my mom this afternoon at six", "en");
  CHECK(decision.confident);
  CHECK_FALSE(decision.fromRules);
  CHECK(decision.intent == intent::ToolIntent::ReminderSet);
}

TEST_CASE("only an explicit ask to be reminded can turn a trigger into a reminder")
{
  BuiltCatalog catalog;
  const NullClassifier absent;
  const IntentRouter once(
      {.catalog = catalog.value,
       .model = absent,
       .recurrent = [](const std::string&, const std::string&) { return false; }});

  CHECK(once.decide("apunta que el mecanico llega el martes", "es").intent ==
        intent::ToolIntent::MemorySave);
  CHECK(once.decide("ten en cuenta que la vecina cuida del gato el sabado", "es").intent ==
        intent::ToolIntent::MemorySave);
  CHECK(once.decide("recuerdame que el mecanico llega el martes", "es").intent ==
        intent::ToolIntent::ReminderSet);
}

TEST_CASE("a save trigger that contains a cancellation phrase is still a save")
{
  BuiltCatalog catalog;
  const AlwaysCameraClassifier wrong;
  const IntentRouter router(
      {.catalog = catalog.value, .model = wrong, .recurrent = nullptr});

  const auto decision = router.decide("don't forget that the dog eats at 7", "en");
  CHECK(decision.confident);
  CHECK(decision.fromRules);
  CHECK(decision.source == intent::DecisionSource::Trigger);
  CHECK(decision.intent != intent::ToolIntent::MemoryForget);
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
  CHECK(intent::normalizeInput("Él llegó. ÁNGEL, ¿qué ÉPOCA?") == "el llego angel que epoca");
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
