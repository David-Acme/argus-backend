#include <doctest/doctest.h>

#include <feature/messenger/services/relay-command.hxx>

#include <optional>
#include <string>

namespace
{

std::optional<relay_command::Command> commandOf(const std::string& utterance)
{
  return relay_command::commandFor(utterance);
}

std::string recipientOf(const relay_command::Command& command)
{
  return command.arguments.get("recipient", "").asString();
}

std::string messageOf(const relay_command::Command& command)
{
  return command.arguments.get("message", "").asString();
}

}

TEST_CASE("the wake word and 'llama a ... y dile que ...' give a relay to the named member with the sender's words")
{
  const auto command = commandOf("Argus, llama a David y dile que venga a comer");
  REQUIRE(command.has_value());
  CHECK(command->tool == "message.relay");
  CHECK(recipientOf(*command) == "david");
  CHECK(messageOf(*command) == "venga a comer");
  CHECK_FALSE(command->reply);
  CHECK(command->fill.empty());
}

TEST_CASE("a plain call request without the 'dile' part is not a relay")
{
  CHECK_FALSE(commandOf("llama a David").has_value());
  CHECK_FALSE(commandOf("llama al 911").has_value());
  CHECK_FALSE(commandOf("recuerdame llamar a David").has_value());
}

TEST_CASE("a relay with no message asks for the message and keeps the recipient")
{
  const auto command = commandOf("llama a David y dile");
  REQUIRE(command.has_value());
  CHECK(recipientOf(*command) == "david");
  CHECK(messageOf(*command).empty());
  REQUIRE(command->fill.size() == 1);
  CHECK(command->fill.front() == "message");
}

TEST_CASE("'dile a ... que ...' names the member and keeps the message's case")
{
  const auto command = commandOf("dile a Ana que Ya voy");
  REQUIRE(command.has_value());
  CHECK(recipientOf(*command) == "ana");
  CHECK(messageOf(*command) == "Ya voy");
  CHECK(command->fill.empty());
}

TEST_CASE("'avisale a ... que ...' with an accent folds to the same family")
{
  const auto command = commandOf("Avísale a Ana que ya llego");
  REQUIRE(command.has_value());
  CHECK(recipientOf(*command) == "ana");
  CHECK(messageOf(*command) == "ya llego");
}

TEST_CASE("'tell ... that ...' is the English form of the same relay")
{
  const auto command = commandOf("tell Ana that dinner is ready");
  REQUIRE(command.has_value());
  CHECK(recipientOf(*command) == "ana");
  CHECK(messageOf(*command) == "dinner is ready");
}

TEST_CASE("a multi-word name is kept whole and a name of more than three words is not a relay")
{
  const auto command = commandOf("dile a Juan Carlos Perez que si");
  REQUIRE(command.has_value());
  CHECK(recipientOf(*command) == "juan carlos perez");
  CHECK(messageOf(*command) == "si");
  CHECK_FALSE(commandOf("dile a cuatro nombres de mas que hola").has_value());
}

TEST_CASE("the message keeps every word after the connector, including a later 'que'")
{
  const auto command = commandOf("dile a ana que venga y que traiga pan");
  REQUIRE(command.has_value());
  CHECK(messageOf(*command) == "venga y que traiga pan");
}

TEST_CASE("'dile que ...' without a name is an answer that may travel back on an open thread")
{
  const auto command = commandOf("dile que ya voy");
  REQUIRE(command.has_value());
  CHECK(command->reply);
  CHECK(recipientOf(*command).empty());
  CHECK(messageOf(*command) == "ya voy");
  REQUIRE(command->fill.size() == 1);
  CHECK(command->fill.front() == "recipient");
}

TEST_CASE("a negated or unrelated utterance is not a relay")
{
  CHECK_FALSE(commandOf("no le digas a Ana nada").has_value());
  CHECK_FALSE(commandOf("No, dile a Ana que nada").has_value());
  CHECK_FALSE(commandOf("que tiempo hace hoy").has_value());
  CHECK_FALSE(commandOf("").has_value());
}

TEST_CASE("'mándale a ... que', 'cuéntale a ... que' and 'let ... know that' are relays too")
{
  const auto send = commandOf("mándale a Lucía que ya llegué");
  REQUIRE(send.has_value());
  CHECK(recipientOf(*send) == "lucia");
  CHECK(messageOf(*send) == "ya llegué");

  const auto tell = commandOf("cuéntale a David que ganamos");
  REQUIRE(tell.has_value());
  CHECK(recipientOf(*tell) == "david");
  CHECK(messageOf(*tell) == "ganamos");

  const auto english = commandOf("let Ana know that dinner is ready");
  REQUIRE(english.has_value());
  CHECK(recipientOf(*english) == "ana");
  CHECK(messageOf(*english) == "dinner is ready");
}
