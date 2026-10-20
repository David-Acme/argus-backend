#include <doctest/doctest.h>

#include <feature/memory/services/memory/memory-scope-classifier.hxx>

namespace
{
MemoryScope scopeOf(std::string_view text, std::string_view lang = "es")
{
  return classifyMemoryScope({.text = text, .lang = lang});
}
}

TEST_CASE("a fact with no everyone cue is personal, whatever its subject")
{
  CHECK(scopeOf("a David le gusta el café") == MemoryScope::Personal);
  CHECK(scopeOf("mi bicicleta es roja") == MemoryScope::Personal);
  CHECK(scopeOf("la cocina tiene una fuga") == MemoryScope::Personal);
  CHECK(scopeOf("el perro se llama Max") == MemoryScope::Personal);
  CHECK(scopeOf("la cámara del cuarto me vio llorar") == MemoryScope::Personal);
  CHECK(scopeOf("the router is in the hall", "en") == MemoryScope::Personal);
}

TEST_CASE("the closing cue makes a fact household whatever its subject")
{
  CHECK(scopeOf("esto es para todos: la cena es a las 8") == MemoryScope::Household);
  CHECK(scopeOf("el wifi es Casa1, esto es para todos") == MemoryScope::Household);
  CHECK(scopeOf("la clave del router es 4421. Esto es para todos.") == MemoryScope::Household);
}

TEST_CASE("'que lo sepan todos' makes a fact household")
{
  CHECK(scopeOf("la puerta del garaje se traba, que lo sepan todos") == MemoryScope::Household);
}

TEST_CASE("'dile a todos' makes a fact household")
{
  CHECK(scopeOf("dile a todos que el perro come a las seis") == MemoryScope::Household);
}

TEST_CASE("'para toda la casa' makes a fact household")
{
  CHECK(scopeOf("el termo es para toda la casa") == MemoryScope::Household);
}

TEST_CASE("the English cues make a fact household")
{
  CHECK(scopeOf("the code is 4421, this is for everyone", "en") == MemoryScope::Household);
  CHECK(scopeOf("the router password is Casa1, let everyone know", "en") == MemoryScope::Household);
  CHECK(scopeOf("the spare key is under the mat, for the whole house", "en") == MemoryScope::Household);
}

TEST_CASE("a loose 'para todos' in an ordinary sentence stays personal")
{
  CHECK(scopeOf("compré regalos para todos mis amigos") == MemoryScope::Personal);
  CHECK(scopeOf("el perro sale es para todos los días") == MemoryScope::Personal);
  CHECK(scopeOf("esto es para todos los días de la semana") == MemoryScope::Personal);
  CHECK(scopeOf("compré un termo para todos") == MemoryScope::Personal);
}

TEST_CASE("the cues follow the language of the utterance and ignore case")
{
  CHECK(scopeOf("this is for everyone", "es") == MemoryScope::Personal);
  CHECK(scopeOf("esto es para todos", "en") == MemoryScope::Personal);
  CHECK(scopeOf("Para Toda La Casa") == MemoryScope::Household);
  CHECK_FALSE(saysForEveryone({.text = "", .lang = "es"}));
}
