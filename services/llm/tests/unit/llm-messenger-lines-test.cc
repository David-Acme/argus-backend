#include <doctest/doctest.h>

#include <feature/messenger/services/relay-lines.hxx>

TEST_CASE("the sender hears what happened in the language asked, and never where anyone is")
{
  CHECK(relay_lines::outcome(RelayOutcome::Delivered, "es") == "Se lo dije.");
  CHECK(relay_lines::outcome(RelayOutcome::Notified, "es") == "Le dejé el mensaje.");
  CHECK(relay_lines::outcome(RelayOutcome::NoAnswer, "es") == "No contestó, le dejé el mensaje.");
  CHECK(relay_lines::outcome(RelayOutcome::NoRecipient, "es") == "No encuentro a nadie con ese nombre.");
  CHECK(relay_lines::outcome(RelayOutcome::Delivered, "en") == "I told them.");
  CHECK(relay_lines::outcome(RelayOutcome::Notified, "en-US") == "I left them the message.");
}

TEST_CASE("a question about the recipient lists the candidates in the language asked")
{
  CHECK(relay_lines::recipientQuestion({}, "es") == "¿A quién se lo digo?");
  CHECK(relay_lines::recipientQuestion({"Ana Lopez", "Ana Ruiz"}, "es") == "¿Se lo digo a Ana Lopez o Ana Ruiz?");
  CHECK(relay_lines::recipientQuestion({"Ana Lopez", "Ana Ruiz"}, "en") == "Should I tell Ana Lopez or Ana Ruiz?");
}

TEST_CASE("a message from a sender keeps the sender's own words")
{
  CHECK(relay_lines::messageFromSender({.sender = "David", .message = "venga a comer", .lang = "es"}) ==
        "David te manda decir: venga a comer");
  CHECK(relay_lines::messageFromSender({.sender = "David", .message = "come here", .lang = "en"}) ==
        "David says: come here");
}

TEST_CASE("the visitor's lines do not depend on who was asked for")
{
  CHECK(relay_lines::visitorAsk("es") == "¿En qué le puedo ayudar?");
  CHECK(relay_lines::visitorDone("es") == "Gracias, se lo hago llegar.");
  CHECK(relay_lines::visitorDone("en") == "Thank you, I will pass it on.");
  CHECK(relay_lines::visitorTitle({.camera = "Sala", .lang = "es"}) == "Un visitante en Sala");
  CHECK(relay_lines::visitorMessage({.camera = "Sala", .need = "traigo el paquete", .lang = "es"}) ==
        "Un visitante en Sala dice: traigo el paquete");
  CHECK(relay_lines::visitorTitle({.camera = "Patio", .lang = "en"}) == "A visitor at Patio");
}
