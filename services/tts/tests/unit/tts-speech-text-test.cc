#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/synthesis/text/number-words.hxx>
#include <feature/synthesis/text/prosodic-chunker.hxx>
#include <feature/synthesis/text/text-normalizer.hxx>

#include <string>
#include <string_view>
#include <vector>

namespace
{
std::string spanish(std::string_view text)
{
  return normalizeSpeechText(text, SpeechLanguage::Spanish);
}

std::string english(std::string_view text)
{
  return normalizeSpeechText(text, SpeechLanguage::English);
}
}

TEST_CASE("the owner's Spanish sentence reads as spoken Spanish")
{
  CHECK(spanish("El Sr. Pérez llega a las 14:30 con el 25 % del pedido, unos 3,5 km después del 1.º de octubre") ==
        "El señor Pérez llega a las dos y media de la tarde con el veinticinco por ciento del pedido, unos tres coma "
        "cinco kilómetros después del primero de octubre");
}

TEST_CASE("Spanish cardinals follow the noun they count")
{
  CHECK(spanishCardinal({.value = 0, .gender = SpanishGender::Standalone}) == "cero");
  CHECK(spanishCardinal({.value = 21, .gender = SpanishGender::Standalone}) == "veintiuno");
  CHECK(spanishCardinal({.value = 100, .gender = SpanishGender::Standalone}) == "cien");
  CHECK(spanishCardinal({.value = 101, .gender = SpanishGender::Standalone}) == "ciento uno");
  CHECK(spanishCardinal({.value = 1000, .gender = SpanishGender::Standalone}) == "mil");
  CHECK(spanishCardinal({.value = 21000, .gender = SpanishGender::Masculine}) == "veintiún mil");
  CHECK(spanishCardinal({.value = 1000000, .gender = SpanishGender::Standalone}) == "un millón");
  CHECK(spanishCardinal({.value = 2500000, .gender = SpanishGender::Standalone}) == "dos millones quinientos mil");
  CHECK(spanish("Hay 1.250 cámaras") == "Hay mil doscientas cincuenta cámaras");
  CHECK(spanish("Tengo 1 perro y 21 personas") == "Tengo un perro y veintiuna personas");
  CHECK(spanish("Vinieron 200 personas") == "Vinieron doscientas personas");
  CHECK(spanish("El 2 de mayo") == "El dos de mayo");
  CHECK(spanish("La temperatura es -5 grados") == "La temperatura es menos cinco grados");
  CHECK(spanish("Pesa 3,25 kg") == "Pesa tres coma veinticinco kilos");
}

TEST_CASE("Spanish money, percentages, units and ordinals")
{
  CHECK(spanish("Cuesta 3,50 €") == "Cuesta tres euros con cincuenta céntimos");
  CHECK(spanish("Cuesta 1 €") == "Cuesta un euro");
  CHECK(spanish("Son $100") == "Son cien dólares");
  CHECK(spanish("Pagó 250 MXN") == "Pagó doscientos cincuenta pesos mexicanos");
  CHECK(spanish("Batería al 7%") == "Batería al siete por ciento");
  CHECK(spanish("Vamos a 90 km/h") == "Vamos a noventa kilómetros por hora");
  CHECK(spanish("Hace 25 °C") == "Hace veinticinco grados");
  CHECK(spanish("Dura 1 h") == "Dura una hora");
  CHECK(spanish("Vive en la 3.ª planta") == "Vive en la tercera planta");
  CHECK(spanish("El 1.er piso") == "El primer piso");
  CHECK(spanishOrdinal({.value = 21, .feminine = false, .apocope = false}) == "vigésimo primero");
}

TEST_CASE("Spanish times and dates")
{
  CHECK(spanish("Llega a las 09:15") == "Llega a las nueve y cuarto de la mañana");
  CHECK(spanish("Llega a la 1:05") == "Llega a la una y cinco");
  CHECK(spanish("Salió a las 00:30") == "Salió a las doce y media de la noche");
  CHECK(spanish("¿Llegas a las 8:00?") == "¿Llegas a las ocho?");
  CHECK(spanish("Son las 21:45") == "Son las nueve y cuarenta y cinco de la noche");
  CHECK(spanish("Cita el 01/10/2026") == "Cita el uno de octubre de dos mil veintiséis");
  CHECK(spanish("Desde 2026-10-03") == "Desde tres de octubre de dos mil veintiséis");
}

TEST_CASE("Spanish abbreviations and symbols keep the prosody marks")
{
  CHECK(spanish("Compré pan, leche, etc.") == "Compré pan, leche, etcétera.");
  CHECK(spanish("La Dra. Gómez y el Ud. de siempre") == "La doctora Gómez y el usted de siempre");
  CHECK(spanish("¡Alarma en la cámara #2!") == "¡Alarma en la cámara número dos!");
  CHECK(spanish("Juan & María **ya** llegaron") == "Juan y María ya llegaron");
}

TEST_CASE("English essentials")
{
  CHECK(english("Dr. Smith arrives at 2:30 pm with 25% of the order, about 3.5 km after October 1st.") ==
        "doctor Smith arrives at two thirty in the afternoon with twenty-five percent of the order, about three "
        "point five kilometers after October first.");
  CHECK(english("It costs $3.50") == "It costs three dollars and fifty cents");
  CHECK(english("We counted 1,250 visitors") == "We counted one thousand two hundred fifty visitors");
  CHECK(english("Due 10/01/2026") == "Due October first, twenty twenty-six");
  CHECK(english("Gate No. 5 is open") == "Gate number five is open");
  CHECK(english("The meeting is at 9:05") == "The meeting is at nine oh five");
  CHECK(englishOrdinal(22) == "twenty-second");
  CHECK(englishYear(1905) == "nineteen oh five");
}

TEST_CASE("other languages pass through untouched")
{
  CHECK(normalizeSpeechText("Il est 14:30 et 25 %", SpeechLanguage::Other) == "Il est 14:30 et 25 %");
  CHECK(speechLanguage("es") == SpeechLanguage::Spanish);
  CHECK(speechLanguage("fr") == SpeechLanguage::Other);
}

TEST_CASE("prosodic chunks keep whole sentences under the limit")
{
  const auto chunks = chunkProsodic({.text = "Hola. Soy Argus. El Sr. Pérez llegó.", .maxUnits = 40, .measure = {}});
  REQUIRE(chunks.size() == 1);
  CHECK(chunks.front() == "Hola. Soy Argus. El Sr. Pérez llegó.");
  const auto split = chunkProsodic({.text = "Primera frase completa. Segunda frase completa.", .maxUnits = 25, .measure = {}});
  REQUIRE(split.size() == 2);
  CHECK(split[0] == "Primera frase completa.");
  CHECK(split[1] == "Segunda frase completa.");
}

TEST_CASE("an oversized sentence breaks at clauses, then commas, never mid-word")
{
  const std::string text = "Las cámaras grabaron a dos visitantes en la puerta; el primero tocó el timbre, dejó una "
                           "nota en el buzón y se fue caminando hacia la calle principal";
  const auto chunks = chunkProsodic({.text = text, .maxUnits = 60, .measure = {}});
  REQUIRE(chunks.size() >= 3);
  CHECK(chunks[0] == "Las cámaras grabaron a dos visitantes en la puerta;");
  CHECK(chunks[1] == "el primero tocó el timbre,");
  for (const auto& chunk : chunks) {
    CHECK(codepointCount(chunk) <= 60);
    CHECK(text.find(chunk) != std::string::npos);
  }
}

TEST_CASE("paragraphs never merge and a custom measure drives the budget")
{
  const auto words = [](std::string_view text) {
    std::size_t count = 1;
    for (const char value : text)
      count += value == ' ' ? 1 : 0;
    return count;
  };
  const auto chunks = chunkProsodic({.text = "uno dos tres cuatro cinco seis\n\nsiete ocho", .maxUnits = 4, .measure = words});
  REQUIRE(chunks.size() == 3);
  CHECK(chunks[0] == "uno dos tres cuatro");
  CHECK(chunks[1] == "cinco seis");
  CHECK(chunks[2] == "siete ocho");
  CHECK(codepointCount("año") == 3);
  const auto hard = chunkProsodic({.text = "supercalifragilístico", .maxUnits = 8, .measure = {}});
  REQUIRE(hard.size() == 3);
  CHECK(hard[0] == "supercal");
}
