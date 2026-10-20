#include <doctest/doctest.h>

#include <feature/proximity/services/whereabouts.hxx>

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
constexpr int64_t kNowMs = 1'800'000'000'000;
constexpr int64_t kOwner = 1;
constexpr int64_t kDavid = 2;
constexpr int64_t kAna = 3;
constexpr int64_t kLuis = 4;
constexpr int64_t kMarta = 5;

WhereaboutsFacts household()
{
  WhereaboutsFacts facts;
  facts.members = {{.userId = kOwner, .name = "Rosa", .role = UserRole::Owner, .active = true},
                   {.userId = kDavid, .name = "David Acme", .role = UserRole::Resident, .active = true},
                   {.userId = kAna, .name = "Ana", .role = UserRole::Resident, .active = true},
                   {.userId = kLuis, .name = "Luis", .role = UserRole::Guest, .active = true},
                   {.userId = kMarta, .name = "Marta", .role = UserRole::Resident, .active = true}};
  facts.verdicts = {{.userId = kDavid,
                     .room = "1:living",
                     .confidence = 0.9,
                     .sinceMs = kNowMs - 300000,
                     .updatedMs = kNowMs - 10000,
                     .strongest = ProximityEvidenceKind::Face}};
  facts.located = std::unordered_set<int64_t>{kOwner, kDavid, kAna, kLuis};
  facts.presence = {{.userId = kAna, .state = PresenceState::Home, .since = kNowMs / 1000 - 600},
                    {.userId = kLuis, .state = PresenceState::Away, .since = kNowMs / 1000 - 3 * 3600}};
  facts.places = {{.id = 1, .name = "Casa"}};
  facts.nowMs = kNowMs;
  return facts;
}

WhereaboutsVoiceReply ask(const WhereaboutsAsk& asked, const WhereaboutsFacts& facts)
{
  return whereabouts::answer({.ask = asked, .facts = facts});
}

WhereaboutsAsk person(const std::string& name, const std::string& lang = "es")
{
  return {.askerId = kOwner, .lang = lang, .person = name, .room = {}};
}

WhereaboutsAsk room(const std::string& name, const std::string& lang = "es")
{
  return {.askerId = kOwner, .lang = lang, .person = {}, .room = name};
}
}

TEST_CASE("a located member is answered with the room and how sure Argus is")
{
  const WhereaboutsFacts facts = household();
  const auto reply = ask(person("david"), facts);
  CHECK(reply.ok);
  CHECK(reply.text == "David Acme está en la sala. Estoy seguro: por la cámara, ahora mismo.");
  CHECK(reply.structured["people"][0]["room"].asString() == "1:living");
  CHECK(reply.structured["people"][0]["certainty"].asString() == "sure");
  CHECK(reply.structured["people"][0]["evidence"].asString() == "face");

  const auto english = ask(person("David", "en"), facts);
  CHECK(english.text == "David Acme is in the living room. I am sure: from the camera, just now.");
}

TEST_CASE("certainty falls with a thin share and with old evidence")
{
  CHECK(whereabouts::certaintyOf({.confidence = 0.9, .ageMs = 1000}) == WhereaboutsCertainty::Sure);
  CHECK(whereabouts::certaintyOf({.confidence = 0.6, .ageMs = 1000}) == WhereaboutsCertainty::Fairly);
  CHECK(whereabouts::certaintyOf({.confidence = 0.4, .ageMs = 1000}) == WhereaboutsCertainty::Unsure);
  CHECK(whereabouts::certaintyOf({.confidence = 0.9, .ageMs = 180000}) == WhereaboutsCertainty::Fairly);
  CHECK(whereabouts::certaintyOf({.confidence = 0.6, .ageMs = 180000}) == WhereaboutsCertainty::Unsure);

  WhereaboutsFacts facts = household();
  facts.verdicts.front().confidence = 0.55;
  facts.verdicts.front().updatedMs = kNowMs - 240000;
  facts.verdicts.front().strongest = ProximityEvidenceKind::Ble;
  CHECK(ask(person("David"), facts).text ==
        "David Acme está en la sala. No estoy muy seguro: por su teléfono, hace 4 min.");
}

TEST_CASE("without a room, presence says home or away")
{
  const WhereaboutsFacts facts = household();
  CHECK(ask(person("Ana"), facts).text == "Ana está en casa, pero no sé en qué parte.");
  CHECK(ask(person("Luis"), facts).text == "Luis no está en casa desde hace 3 h.");
  CHECK(ask(person("Luis", "en"), facts).text == "Luis has been away for 3 h.");
}

TEST_CASE("a member who turned location off is never placed in a room")
{
  WhereaboutsFacts facts = household();
  facts.verdicts.push_back({.userId = kMarta,
                            .room = "1:kitchen",
                            .confidence = 0.95,
                            .sinceMs = kNowMs,
                            .updatedMs = kNowMs,
                            .strongest = ProximityEvidenceKind::Face});
  facts.located->erase(kMarta);
  const auto reply = ask(person("Marta"), facts);
  CHECK(reply.text == "No sé dónde está Marta: no comparte su ubicación con Argus.");
  CHECK(reply.text.find("cocina") == std::string::npos);
  CHECK_FALSE(reply.structured["people"][0]["sharesLocation"].asBool());

  facts.presence.push_back({.userId = kMarta, .state = PresenceState::Home, .since = 0});
  CHECK(ask(person("Marta"), facts).text ==
        "Marta está en casa, pero no sé en qué parte. No comparte su ubicación dentro de casa.");
}

TEST_CASE("an unknown or shared name is never guessed")
{
  WhereaboutsFacts facts = household();
  const auto missing = ask(person("Pedro"), facts);
  CHECK_FALSE(missing.ok);
  CHECK(missing.code == "unknown_person");
  CHECK(missing.text == "No conozco a nadie de casa llamado Pedro.");

  facts.members.push_back({.userId = 9, .name = "Ana Sofía", .role = UserRole::Resident, .active = true});
  facts.members.push_back({.userId = 10, .name = "Ana Lucía", .role = UserRole::Resident, .active = true});
  facts.members.erase(facts.members.begin() + 2);
  const auto shared = ask(person("Ana"), facts);
  CHECK_FALSE(shared.ok);
  CHECK(shared.code == "ambiguous_person");
}

TEST_CASE("an inactive account is not a household member")
{
  WhereaboutsFacts facts = household();
  facts.members.at(2).active = false;
  CHECK(ask(person("Ana"), facts).code == "unknown_person");
}

TEST_CASE("a room question lists who is there, and says when it only counts those who share")
{
  WhereaboutsFacts facts = household();
  const auto living = ask(room("sala"), facts);
  CHECK(living.ok);
  CHECK(living.text == "En la sala está David Acme. Estoy seguro. Solo cuento a quien comparte su ubicación.");
  CHECK(living.structured["room"].asString() == "living");

  facts.verdicts.push_back({.userId = kAna,
                            .room = "1:living",
                            .confidence = 0.6,
                            .sinceMs = kNowMs,
                            .updatedMs = kNowMs,
                            .strongest = ProximityEvidenceKind::Voice});
  facts.located->insert(kMarta);
  CHECK(ask(room("la sala"), facts).text == "En la sala están David Acme y Ana. Estoy bastante seguro.");
  CHECK(ask(room("living room", "en"), facts).text == "In the living room are David Acme and Ana. I am fairly sure.");

  CHECK(ask(room("cocina"), facts).text == "No detecto a nadie de casa en la cocina.");
  const auto unknown = ask(room("azotea"), facts);
  CHECK_FALSE(unknown.ok);
  CHECK(unknown.code == "unknown_room");
}

TEST_CASE("the asker is left out of a room and of the household")
{
  WhereaboutsFacts facts = household();
  facts.verdicts.front().userId = kOwner;
  facts.located->insert(kMarta);
  CHECK(ask(room("sala"), facts).text == "No detecto a nadie de casa en la sala.");
}

TEST_CASE("who is home groups the household by room, home, away and unknown")
{
  const WhereaboutsFacts facts = household();
  const auto reply = ask({.askerId = kOwner, .lang = "es", .person = {}, .room = {}}, facts);
  CHECK(reply.text == "En casa: David Acme (en la sala) y Ana. Fuera: Luis. No sé dónde están: Marta.");
  CHECK(reply.structured["people"].size() == 4);
}

TEST_CASE("room keys read as rooms, cameras and places")
{
  WhereaboutsFacts facts = household();
  facts.cameras = {{.id = 7, .name = "Pasillo"}};
  CHECK(whereabouts::roomLabel({.room = "1:kitchen", .english = false, .facts = facts}) == "en la cocina");
  CHECK(whereabouts::roomLabel({.room = "1:camera:7", .english = false, .facts = facts}) == "cerca de la cámara Pasillo");
  CHECK(whereabouts::roomLabel({.room = "1:camera:7", .english = true, .facts = facts}) == "near the Pasillo camera");
  facts.places.push_back({.id = 2, .name = "Casa de campo"});
  CHECK(whereabouts::roomLabel({.room = "2:garage", .english = false, .facts = facts}) == "en el garaje de Casa de campo");
  CHECK(whereabouts::roomRoleOf("la cochera") == std::optional<std::string>("garage"));
  CHECK(whereabouts::roomRoleOf("Living Room") == std::optional<std::string>("living"));
  CHECK_FALSE(whereabouts::roomRoleOf("baño").has_value());
}

TEST_CASE("the service answers only the owner, and says so when the household cannot be read")
{
  const WhereaboutsService refusing({.members = {},
                                     .verdicts = {},
                                     .located = {},
                                     .presence = {},
                                     .places = {},
                                     .cameraName = {},
                                     .clockMs = {}});
  const auto resident = drogon::sync_wait(
      refusing.answer({.userId = kAna, .role = "resident", .lang = "es", .person = "David", .room = {}}));
  CHECK_FALSE(resident.ok);
  CHECK(resident.code == "forbidden");
  const auto owner = drogon::sync_wait(
      refusing.answer({.userId = kOwner, .role = "owner", .lang = "es", .person = "David", .room = {}}));
  CHECK_FALSE(owner.ok);
  CHECK(owner.code == "whereabouts_unavailable");
}
