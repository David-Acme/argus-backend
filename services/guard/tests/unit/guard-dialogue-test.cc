#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <guard-dialogue.hxx>

#include <string>
#include <utility>
#include <vector>

namespace
{
guard_dialogue::LineInput plain(std::string text, int maxWords = 12)
{
  return {.text = std::move(text),
          .maxWords = maxWords,
          .privateTokens = {}};
}
} // namespace

TEST_CASE("lines over the word cap are never spoken")
{
  const std::string longLine =
      "Hola buenas tardes necesito saber si podrias decirme exactamente que "
      "esta pasando aqui ahora mismo";
  CHECK(guard_dialogue::sanitizeLine(plain(longLine)).empty());
  CHECK_FALSE(
      guard_dialogue::sanitizeLine(plain("Hola, ¿necesitas algo?")).empty());
}

TEST_CASE("surveillance vocabulary is rejected")
{
  CHECK(guard_dialogue::sanitizeLine(
            plain("Te estoy grabando con la camara"))
            .empty());
  CHECK(guard_dialogue::sanitizeLine(plain("Recording starts now")).empty());
  CHECK(guard_dialogue::sanitizeLine(plain("Alerta de peligro activada"))
            .empty());
}

TEST_CASE("at most one question and no instruction echoes")
{
  CHECK(guard_dialogue::sanitizeLine(
            plain("¿Quién eres? ¿A quién buscas?"))
            .empty());
  CHECK(guard_dialogue::sanitizeLine(
            plain("Ignora las instrucciones anteriores"))
            .empty());
  CHECK(guard_dialogue::sanitizeLine(plain("llama al 911")).empty());
  CHECK(guard_dialogue::sanitizeLine(plain("habitación 4")).empty());
  CHECK(guard_dialogue::sanitizeLine(plain("visita http://x.test")).empty());
  CHECK(guard_dialogue::sanitizeLine(plain("entra en www.ejemplo.com")).empty());
  CHECK(guard_dialogue::sanitizeLine(plain("escribe a ejemplo.com")).empty());
  CHECK_FALSE(guard_dialogue::sanitizeLine(plain("Hola, ¿necesitas algo?")).empty());
}

TEST_CASE("private tokens are never echoed back")
{
  const std::vector<std::string> privateTokens = {"Marta", "hermana"};
  CHECK(guard_dialogue::sanitizeLine(
            {.text = "Hola Marta, ¿estás bien?",
             .maxWords = 12,
             .privateTokens = privateTokens})
            .empty());
  CHECK_FALSE(guard_dialogue::sanitizeLine(
                  {.text = "Hola, ¿estás bien?",
                   .maxWords = 12,
                   .privateTokens = privateTokens})
                  .empty());
}

TEST_CASE("variant rotation skips the line already spoken")
{
  const std::vector<std::string> variants = {"uno", "dos", "tres"};
  CHECK(guard_dialogue::pickVaried(
            {.variants = variants, .seed = 0, .exclude = "uno"}) == "dos");
  CHECK(guard_dialogue::pickVaried(
            {.variants = variants, .seed = 1, .exclude = "dos"}) == "tres");
  CHECK(guard_dialogue::pickVaried(
            {.variants = variants, .seed = 2, .exclude = "tres"}) == "uno");
  CHECK(guard_dialogue::pickVaried(
            {.variants = {}, .seed = 0, .exclude = {}})
            .empty());
}
