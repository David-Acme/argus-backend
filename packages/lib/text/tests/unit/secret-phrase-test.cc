#include <doctest/doctest.h>
#include <text/secret-phrase.hxx>
#include <text/sha256.hxx>

#include <string>
#include <string_view>
#include <vector>

namespace
{
std::string hexOf(const secret_phrase::Digest& digest)
{
  return argus::hash::hex(digest);
}

std::string testSalt()
{
  std::string salt;
  for (char value = 0; value < 16; ++value)
    salt.push_back(value);
  return salt;
}

secret_phrase::PhraseKey keyOf(std::string_view phrase)
{
  const auto key =
      secret_phrase::seal({.phrase = phrase, .salt = testSalt(), .iterations = secret_phrase::kMinIterations});
  REQUIRE(key.has_value());
  return key.value_or(secret_phrase::PhraseKey{});
}
}

TEST_CASE("pbkdf2 over HMAC-SHA256 matches the reference vectors")
{
  CHECK(hexOf(secret_phrase::pbkdf2Sha256({.password = "passwd", .salt = "salt", .iterations = 1})) ==
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");
  CHECK(hexOf(secret_phrase::pbkdf2Sha256({.password = "password", .salt = "salt", .iterations = 1})) ==
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
  CHECK(hexOf(secret_phrase::pbkdf2Sha256({.password = "password", .salt = "salt", .iterations = 2})) ==
        "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
  CHECK(hexOf(secret_phrase::pbkdf2Sha256({.password = "password", .salt = "salt", .iterations = 4096})) ==
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
  const std::string longKey(100, 'k');
  CHECK(hexOf(secret_phrase::pbkdf2Sha256({.password = longKey, .salt = "salt", .iterations = 3})) ==
        "219c68bb88f1010f4a763b7c8fd58639c2280baf6820814c6d404891fcd85c3d");
}

TEST_CASE("words fold case, accents and punctuation the same way on both sides")
{
  CHECK(secret_phrase::words("¿El GATO está en el tejado?") ==
        std::vector<std::string>{"el", "gato", "esta", "en", "el", "tejado"});
  CHECK(secret_phrase::words("Ñandú, ¡ya!") == std::vector<std::string>{"nandu", "ya"});
  CHECK(secret_phrase::words("«la tía—Rosa» no viene…") ==
        std::vector<std::string>{"la", "tia", "rosa", "no", "viene"});
  CHECK(secret_phrase::words("Straße  über") == std::vector<std::string>{"strasse", "uber"});
  CHECK(secret_phrase::words("tengo 3 gatos") == std::vector<std::string>{"tengo", "3", "gatos"});
  CHECK(secret_phrase::words("  ,;  ").empty());
  CHECK(secret_phrase::words(std::string_view("bad \xff\xfe utf8")).empty());
}

TEST_CASE("a sealed phrase is the PBKDF2 of its folded words and refuses bad input")
{
  CHECK(hexOf(keyOf("El gato está en el tejado").digest) ==
        "3d7b245ecad96375e10ddd9154bb8cc55683dcabcb30755963adebc7d1bef9a6");
  CHECK(keyOf("El gato está en el tejado").words == 6);
  CHECK_FALSE(secret_phrase::seal({.phrase = "dos palabras", .salt = testSalt(), .iterations = 1024}));
  CHECK_FALSE(secret_phrase::seal({.phrase = "uno dos tres cuatro cinco seis siete ocho nueve diez once doce trece",
                                   .salt = testSalt(),
                                   .iterations = 1024}));
  CHECK_FALSE(secret_phrase::seal({.phrase = "el gato pardo", .salt = "short", .iterations = 1024}));
  const std::string fifteen = testSalt().substr(0, 15);
  CHECK_FALSE(
      secret_phrase::seal({.phrase = "el gato pardo", .salt = fifteen, .iterations = secret_phrase::kMinIterations}));
  CHECK_FALSE(secret_phrase::seal({.phrase = "el gato pardo",
                                   .salt = "context-scenarios-duress",
                                   .iterations = secret_phrase::kMinIterations}));
  CHECK_FALSE(secret_phrase::seal({.phrase = "el gato pardo", .salt = testSalt(), .iterations = 1000}));
}

TEST_CASE("a key survives encoding and a malformed one is refused")
{
  const secret_phrase::PhraseKey key = keyOf("la tía Rosa no viene");
  const std::string stored = secret_phrase::encode(key);
  CHECK(stored.starts_with("phrase-pbkdf2-sha256$1024$5$000102030405060708090a0b0c0d0e0f$"));
  CHECK(stored.find("rosa") == std::string::npos);
  REQUIRE(secret_phrase::decode(stored).has_value());
  const secret_phrase::PhraseKey decoded = secret_phrase::decode(stored).value_or(secret_phrase::PhraseKey{});
  CHECK(decoded.iterations == key.iterations);
  CHECK(decoded.words == key.words);
  CHECK(decoded.salt == key.salt);
  CHECK(decoded.digest == key.digest);

  CHECK_FALSE(secret_phrase::decode(""));
  CHECK_FALSE(secret_phrase::decode("pbkdf2-sha256$1024$5$00$00"));
  CHECK_FALSE(secret_phrase::decode("phrase-pbkdf2-sha256$1024$5$0001$" + std::string(64, 'a')));
  CHECK_FALSE(secret_phrase::decode("phrase-pbkdf2-sha256$100$5$000102030405060708090a0b0c0d0e0f$" +
                                    std::string(64, 'a')));
  CHECK_FALSE(secret_phrase::decode("phrase-pbkdf2-sha256$1024$2$000102030405060708090a0b0c0d0e0f$" +
                                    std::string(64, 'a')));
  CHECK_FALSE(secret_phrase::decode("phrase-pbkdf2-sha256$1024$5$000102030405060708090a0b0c0d0e0f$" +
                                    std::string(64, 'g')));
  CHECK_FALSE(secret_phrase::decode(stored + "$extra"));
}

TEST_CASE("the phrase is found inside an utterance however the transcript spells it")
{
  const secret_phrase::PhraseKey key = keyOf("El gato está en el tejado");
  CHECK(secret_phrase::spokenIn(key, "el gato esta en el tejado"));
  CHECK(secret_phrase::spokenIn(key, "Argus, ¿sabes? El gato está, en el tejado. Qué raro."));
  CHECK(secret_phrase::spokenIn(key, "EL GATO ESTÁ EN EL TEJADO"));
  CHECK_FALSE(secret_phrase::spokenIn(key, "el gato está en tejado"));
  CHECK_FALSE(secret_phrase::spokenIn(key, "el tejado está en el gato"));
  CHECK_FALSE(secret_phrase::spokenIn(key, "los gatos están en el tejado"));
  CHECK_FALSE(secret_phrase::spokenIn(key, ""));
}

TEST_CASE("only the tail of a long utterance is screened")
{
  const secret_phrase::PhraseKey key = keyOf("el gato pardo");
  std::string padding;
  for (int word = 0; word < 120; ++word)
    padding += " palabra";
  CHECK(secret_phrase::spokenIn(key, padding + " el gato pardo"));
  CHECK_FALSE(secret_phrase::spokenIn(key, "el gato pardo" + padding));
}
