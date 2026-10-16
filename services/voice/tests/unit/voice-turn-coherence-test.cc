#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/voice/turn-coherence.hxx>

TEST_CASE("speech with a content word proceeds, addressed or not")
{
  CHECK(turnCoherenceOf("hola argus, cambia la luz del patio") == TurnCoherence::Proceed);
  CHECK(turnCoherenceOf("the weather is nice today") == TurnCoherence::Proceed);
  CHECK(turnCoherenceOf("ya lo sé, no te preocupes") == TurnCoherence::Proceed);
}

TEST_CASE("low-signal speech that addresses Argus gets one brief question, never a request")
{
  CHECK(turnCoherenceOf("Argus") == TurnCoherence::AskAddressed);
  CHECK(turnCoherenceOf("argus, eh pues") == TurnCoherence::AskAddressed);
  CHECK(isAddressed("Argus!"));
  CHECK_FALSE(hasContentWords("argus"));
}

TEST_CASE("low-signal speech that does not address Argus gets no action at all")
{
  CHECK(turnCoherenceOf("eh pues ya") == TurnCoherence::Ignore);
  CHECK(turnCoherenceOf("sí") == TurnCoherence::Ignore);
  CHECK(turnCoherenceOf("") == TurnCoherence::Ignore);
  CHECK(turnCoherenceOf("uh, um, ok") == TurnCoherence::Ignore);
  CHECK_FALSE(isAddressed("eh pues ya"));
}

TEST_CASE("addressing is a whole word, not a substring")
{
  CHECK_FALSE(isAddressed("argüello dijo algo"));
  CHECK_FALSE(isAddressed("argusos son bonitos"));
  CHECK(isAddressed("¿Argus, qué hora es?"));
}
