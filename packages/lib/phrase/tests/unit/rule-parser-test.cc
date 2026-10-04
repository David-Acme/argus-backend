#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <phrase/phrase-catalog.hxx>
#include <phrase/rule-parser.hxx>

#include <string>

namespace
{
struct Parser
{
  PhraseCatalog catalog;
  RuleParser parser{catalog};

  Parser() { catalog.build(); }

  bool statement(const std::string& text, const std::string& lang) const
  {
    return parser.parseStatement({.text = text, .lang = lang}).has_value();
  }
};
}

TEST_CASE("a command that opens the clause is not a statement about its object")
{
  const Parser p;
  CHECK_FALSE(p.statement("muéstrame la cámara 3", "es"));
  CHECK_FALSE(p.statement("Muéstrame la cámara del garaje", "es"));
  CHECK_FALSE(p.statement("enciende la luz de la cocina", "es"));
  CHECK_FALSE(p.statement("apaga la calefacción del salón", "es"));
  CHECK_FALSE(p.statement("oye, enséñame la entrada ahora", "es"));
  CHECK_FALSE(p.statement("oye ábreme la puerta del garaje", "es"));
  CHECK_FALSE(p.statement("quiero ver la cámara del patio", "es"));
  CHECK_FALSE(p.statement("puedes apagar la luz del pasillo", "es"));
  CHECK_FALSE(p.statement("por favor pon la alarma a las siete", "es"));
  CHECK_FALSE(p.statement("explícame la alarma de incendios", "es"));
  CHECK_FALSE(p.statement("show me the camera 3 please", "en"));
  CHECK_FALSE(p.statement("turn off the heating tonight", "en"));
  CHECK_FALSE(p.statement("can you check the front door", "en"));
}

TEST_CASE("statements that do not open with a command still parse")
{
  const Parser p;
  CHECK(p.statement("mi hermana viene los domingos", "es"));
  CHECK(p.statement("oye, la cena es a las ocho", "es"));
  CHECK(p.statement("por cierto la cena es a las ocho", "es"));
  CHECK(p.statement("voy a ver a mi hermana el lunes", "es"));
  CHECK(p.statement("muéstrame la cámara, mi hermana viene los domingos", "es"));
  CHECK(p.statement("my sister comes on sundays", "en"));
  CHECK(p.statement("I check the mailbox every morning", "en"));
}

TEST_CASE("an explicit trigger is unaffected by the command check")
{
  const Parser p;
  const auto parsed = p.parser.parse({.text = "recuerda que la cámara del garaje se cae", .lang = "es"});
  REQUIRE(parsed.has_value());
  CHECK(parsed.value_or(RuleParseResult{}).content == "la cámara del garaje se cae");
}

TEST_CASE("plain save requests are explicit triggers in both languages")
{
  const Parser p;
  const auto content = [&p](const std::string& text, const std::string& lang) {
    const auto parsed = p.parser.parse({.text = text, .lang = lang});
    return parsed ? parsed->content : std::string("<none>");
  };
  CHECK(content("save that the gate code is 1234", "en") == "the gate code is 1234");
  CHECK(content("please save that my locker number is 17", "en") == "my locker number is 17");
  CHECK(content("save this: the spare key is under the mat", "en") == ": the spare key is under the mat");
  CHECK(content("guárdame esto: la llave está debajo de la maceta", "es") ==
        ": la llave está debajo de la maceta");
  CHECK(content("no te olvides de que la basura sale los jueves", "es") == "la basura sale los jueves");
  CHECK(content("quiero que guardes que mi talla de zapato es la 42", "es") == "mi talla de zapato es la 42");
  CHECK(content("memoriza que mi cumpleaños es el 3 de mayo", "es") == "mi cumpleaños es el 3 de mayo");
  CHECK(content("toma nota, el vecino se llama Pedro", "es") == ", el vecino se llama Pedro");
}

TEST_CASE("an utterance that opens with a command is a command, a reminder verb is not")
{
  const Parser p;
  const auto command = [&p](const std::string& text, const std::string& lang) {
    return p.parser.isCommand({.text = text, .lang = lang});
  };
  CHECK(command("muéstrame la cámara 3", "es"));
  CHECK(command("oye, enciende la luz a las 8", "es"));
  CHECK(command("show me camera 3", "en"));
  CHECK_FALSE(command("recuérdame mañana a las nueve llamar al dentista", "es"));
  CHECK_FALSE(command("mi hermana viene a las 3", "es"));
  CHECK_FALSE(command("remind me at 9 to call the dentist", "en"));
  CHECK(p.statement("recuérdame la cita con el médico del lunes", "es"));
}
