#include "whereabouts.hxx"

#include <runtime/blocking-task.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <map>
#include <utility>

namespace
{
constexpr int64_t kMsPerSecond = 1000;
constexpr int64_t kSecondsPerMinute = 60;
constexpr int64_t kHoursFromMinutes = 120;
constexpr int64_t kMinutesPerHour = 60;
constexpr int64_t kFreshMs = 60000;
constexpr int64_t kStaleMs = 120000;
constexpr double kSureShare = 0.75;
constexpr double kFairShare = 0.5;

struct RoomWords
{
  std::string_view role;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<RoomWords, 7> kRoomLabels{{{.role = "living", .es = "en la sala", .en = "in the living room"},
                                                {.role = "kitchen", .es = "en la cocina", .en = "in the kitchen"},
                                                {.role = "office", .es = "en la oficina", .en = "in the office"},
                                                {.role = "garage", .es = "en el garaje", .en = "in the garage"},
                                                {.role = "register", .es = "en la caja", .en = "at the register"},
                                                {.role = "storage", .es = "en el depósito", .en = "in the storage room"},
                                                {.role = "entrance", .es = "en la entrada", .en = "at the entrance"}}};

struct RoomSynonym
{
  std::string_view spoken;
  std::string_view role;
};

constexpr std::array<RoomSynonym, 27> kRoomSynonyms{{{.spoken = "sala", .role = "living"},
                                                     {.spoken = "sala de estar", .role = "living"},
                                                     {.spoken = "salon", .role = "living"},
                                                     {.spoken = "living", .role = "living"},
                                                     {.spoken = "living room", .role = "living"},
                                                     {.spoken = "lounge", .role = "living"},
                                                     {.spoken = "cocina", .role = "kitchen"},
                                                     {.spoken = "kitchen", .role = "kitchen"},
                                                     {.spoken = "oficina", .role = "office"},
                                                     {.spoken = "estudio", .role = "office"},
                                                     {.spoken = "despacho", .role = "office"},
                                                     {.spoken = "office", .role = "office"},
                                                     {.spoken = "study", .role = "office"},
                                                     {.spoken = "garaje", .role = "garage"},
                                                     {.spoken = "garage", .role = "garage"},
                                                     {.spoken = "cochera", .role = "garage"},
                                                     {.spoken = "deposito", .role = "storage"},
                                                     {.spoken = "almacen", .role = "storage"},
                                                     {.spoken = "bodega", .role = "storage"},
                                                     {.spoken = "storage", .role = "storage"},
                                                     {.spoken = "storage room", .role = "storage"},
                                                     {.spoken = "storeroom", .role = "storage"},
                                                     {.spoken = "caja", .role = "register"},
                                                     {.spoken = "register", .role = "register"},
                                                     {.spoken = "entrada", .role = "entrance"},
                                                     {.spoken = "recibidor", .role = "entrance"},
                                                     {.spoken = "entrance", .role = "entrance"}}};

struct RoomKey
{
  int64_t environmentId{0};
  std::string role;
  int64_t cameraId{0};
};

int64_t numberOf(std::string_view text)
{
  int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9')
      return 0;
    value = value * 10 + (c - '0');
  }
  return value;
}

RoomKey keyOf(std::string_view room)
{
  RoomKey key;
  const size_t colon = room.find(':');
  if (colon == std::string_view::npos) {
    key.role = std::string(room);
    return key;
  }
  key.environmentId = numberOf(room.substr(0, colon));
  const std::string_view rest = room.substr(colon + 1);
  constexpr std::string_view kCameraPrefix = "camera:";
  if (rest.starts_with(kCameraPrefix)) {
    key.cameraId = numberOf(rest.substr(kCameraPrefix.size()));
    return key;
  }
  key.role = std::string(rest);
  return key;
}

std::string pick(bool english, std::string es, std::string en)
{
  return english ? std::move(en) : std::move(es);
}

std::string capitalized(std::string text)
{
  if (!text.empty())
    text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
  return text;
}

std::string joinNames(const std::vector<std::string>& names, bool english)
{
  std::string out;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0)
      out += index + 1 == names.size() ? (english ? " and " : " y ") : ", ";
    out += names.at(index);
  }
  return out;
}

std::string agoOf(int64_t ageMs, bool english)
{
  if (ageMs < kFreshMs)
    return pick(english, "ahora mismo", "just now");
  const int64_t minutes = ageMs / kMsPerSecond / kSecondsPerMinute;
  if (minutes >= kHoursFromMinutes)
    return pick(english, "hace " + std::to_string(minutes / kMinutesPerHour) + " h",
                std::to_string(minutes / kMinutesPerHour) + " h ago");
  return pick(english, "hace " + std::to_string(minutes) + " min", std::to_string(minutes) + " min ago");
}

std::string durationOf(int64_t seconds)
{
  const int64_t minutes = std::max<int64_t>(1, seconds / kSecondsPerMinute);
  if (minutes >= kHoursFromMinutes)
    return std::to_string(minutes / kMinutesPerHour) + " h";
  return std::to_string(minutes) + " min";
}

std::string sourceOf(ProximityEvidenceKind kind, bool english)
{
  switch (kind) {
    case ProximityEvidenceKind::Face:
      return pick(english, "por la cámara", "from the camera");
    case ProximityEvidenceKind::RadarBox:
      return pick(english, "por un sensor de presencia y la cámara", "from a presence sensor and the camera");
    case ProximityEvidenceKind::Voice:
      return pick(english, "por su voz", "from their voice");
    case ProximityEvidenceKind::Ble:
      return pick(english, "por su teléfono", "from their phone");
  }
  return {};
}

std::string certaintyWords(WhereaboutsCertainty certainty, bool english)
{
  switch (certainty) {
    case WhereaboutsCertainty::Sure:
      return pick(english, "Estoy seguro", "I am sure");
    case WhereaboutsCertainty::Fairly:
      return pick(english, "Estoy bastante seguro", "I am fairly sure");
    case WhereaboutsCertainty::Unsure:
      return pick(english, "No estoy muy seguro", "I am not very sure");
  }
  return {};
}

std::string certaintyName(WhereaboutsCertainty certainty)
{
  switch (certainty) {
    case WhereaboutsCertainty::Sure:
      return "sure";
    case WhereaboutsCertainty::Fairly:
      return "fairly";
    case WhereaboutsCertainty::Unsure:
      return "unsure";
  }
  return "unsure";
}

WhereaboutsVoiceReply failure(std::string text, std::string code)
{
  return {.ok = false, .text = std::move(text), .code = std::move(code), .structured = Json::Value(Json::objectValue)};
}

bool sharesLocation(const WhereaboutsFacts& facts, int64_t userId)
{
  return !facts.located || facts.located->contains(userId);
}

const RoomVerdict* verdictFor(const WhereaboutsFacts& facts, int64_t userId)
{
  if (!sharesLocation(facts, userId))
    return nullptr;
  const auto found = std::ranges::find(facts.verdicts, userId, &RoomVerdict::userId);
  return found == facts.verdicts.end() ? nullptr : &*found;
}

const WhereaboutsPresence* presenceFor(const WhereaboutsFacts& facts, int64_t userId)
{
  const auto found = std::ranges::find(facts.presence, userId, &WhereaboutsPresence::userId);
  return found == facts.presence.end() ? nullptr : &*found;
}

std::vector<WhereaboutsMember> activeMembers(const WhereaboutsFacts& facts)
{
  std::vector<WhereaboutsMember> members;
  for (const WhereaboutsMember& member : facts.members)
    if (member.active && member.userId > 0 && !member.name.empty())
      members.push_back(member);
  return members;
}

struct VerdictLine
{
  const RoomVerdict& verdict;
  const WhereaboutsFacts& facts;
  bool english{false};
};

WhereaboutsCertainty certaintyOfVerdict(const VerdictLine& line)
{
  return whereabouts::certaintyOf(
      {.confidence = line.verdict.confidence, .ageMs = std::max<int64_t>(0, line.facts.nowMs - line.verdict.updatedMs)});
}

std::string evidenceSentence(const VerdictLine& line)
{
  const int64_t ageMs = std::max<int64_t>(0, line.facts.nowMs - line.verdict.updatedMs);
  return certaintyWords(certaintyOfVerdict(line), line.english) + ": " +
         sourceOf(line.verdict.strongest, line.english) + ", " + agoOf(ageMs, line.english) + ".";
}

struct EntryInput
{
  const WhereaboutsMember& member;
  const WhereaboutsFacts& facts;
  bool english{false};
};

Json::Value entryOf(const EntryInput& input)
{
  const WhereaboutsMember& member = input.member;
  const WhereaboutsFacts& facts = input.facts;
  const bool english = input.english;
  Json::Value entry(Json::objectValue);
  entry["userId"] = static_cast<Json::Int64>(member.userId);
  entry["name"] = member.name;
  entry["sharesLocation"] = sharesLocation(facts, member.userId);
  if (const RoomVerdict* verdict = verdictFor(facts, member.userId)) {
    const VerdictLine line{.verdict = *verdict, .facts = facts, .english = english};
    entry["room"] = verdict->room;
    entry["place"] = whereabouts::roomLabel({.room = verdict->room, .english = english, .facts = facts});
    entry["confidence"] = verdict->confidence;
    entry["certainty"] = certaintyName(certaintyOfVerdict(line));
    entry["evidence"] = proximityEvidenceKindToString(verdict->strongest);
    entry["updatedAt"] = static_cast<Json::Int64>(verdict->updatedMs / kMsPerSecond);
  }
  if (const WhereaboutsPresence* presence = presenceFor(facts, member.userId)) {
    entry["presence"] = presenceStateToString(presence->state);
    entry["presenceSince"] = static_cast<Json::Int64>(presence->since);
  }
  return entry;
}

WhereaboutsVoiceReply personAnswer(const WhereaboutsAsk& ask, const WhereaboutsFacts& facts)
{
  const bool english = ask.lang == "en";
  const std::vector<WhereaboutsMember> members = activeMembers(facts);
  std::vector<std::string> names;
  names.reserve(members.size());
  for (const WhereaboutsMember& member : members)
    names.push_back(member.name);
  const text_norm::NameMatch match = text_norm::matchName(names, ask.person);
  if (match.kind == text_norm::NameMatchKind::Missing)
    return failure(pick(english, "No conozco a nadie de casa llamado " + ask.person + ".",
                        "I do not know anyone at home called " + ask.person + "."),
                   "unknown_person");
  if (match.kind == text_norm::NameMatchKind::Ambiguous) {
    std::vector<std::string> candidates;
    for (const size_t index : match.hits)
      candidates.push_back(names.at(index));
    return failure(pick(english, "¿A quién te refieres? " + joinNames(candidates, false) + ".",
                        "Who do you mean? " + joinNames(candidates, true) + "."),
                   "ambiguous_person");
  }
  const WhereaboutsMember& member = members.at(match.hits.front());
  WhereaboutsVoiceReply reply{.ok = true, .text = {}, .code = {}, .structured = Json::Value(Json::objectValue)};
  Json::Value people(Json::arrayValue);
  people.append(entryOf({.member = member, .facts = facts, .english = english}));
  reply.structured["people"] = std::move(people);
  if (const RoomVerdict* verdict = verdictFor(facts, member.userId)) {
    const VerdictLine line{.verdict = *verdict, .facts = facts, .english = english};
    const std::string place = whereabouts::roomLabel({.room = verdict->room, .english = english, .facts = facts});
    reply.text = pick(english, member.name + " está " + place + ". ", member.name + " is " + place + ". ") +
                 evidenceSentence(line);
    return reply;
  }
  const bool shares = sharesLocation(facts, member.userId);
  if (const WhereaboutsPresence* presence = presenceFor(facts, member.userId)) {
    if (presence->state == PresenceState::Home) {
      reply.text = pick(english, member.name + " está en casa, pero no sé en qué parte.",
                        member.name + " is home, but I do not know where in the house.");
      if (!shares)
        reply.text += pick(english, " No comparte su ubicación dentro de casa.",
                           " They do not share their location inside the house.");
      return reply;
    }
    if (presence->state == PresenceState::Away) {
      const int64_t nowS = facts.nowMs / kMsPerSecond;
      if (presence->since > 0 && presence->since <= nowS)
        reply.text = pick(english,
                          member.name + " no está en casa desde hace " + durationOf(nowS - presence->since) +
                              ".",
                          member.name + " has been away for " + durationOf(nowS - presence->since) + ".");
      else
        reply.text = pick(english, member.name + " no está en casa.", member.name + " is not home.");
      return reply;
    }
  }
  if (!shares) {
    reply.text = pick(english, "No sé dónde está " + member.name + ": no comparte su ubicación con Argus.",
                      "I do not know where " + member.name + " is: they do not share their location with Argus.");
    return reply;
  }
  reply.text = pick(english, "No sé dónde está " + member.name + " ahora mismo.",
                    "I do not know where " + member.name + " is right now.");
  return reply;
}

std::string roleOfRoom(std::string_view room)
{
  return keyOf(room).role;
}

bool someoneHidden(const WhereaboutsFacts& facts, int64_t askerId)
{
  if (!facts.located)
    return false;
  return std::ranges::any_of(activeMembers(facts), [&facts, askerId](const WhereaboutsMember& member) {
    return member.userId != askerId && !facts.located->contains(member.userId);
  });
}

WhereaboutsVoiceReply roomAnswer(const WhereaboutsAsk& ask, const WhereaboutsFacts& facts)
{
  const bool english = ask.lang == "en";
  const std::optional<std::string> role = whereabouts::roomRoleOf(ask.room);
  if (!role)
    return failure(pick(english, "No sé qué habitación es " + ask.room + ".",
                        "I do not know which room " + ask.room + " is."),
                   "unknown_room");
  std::map<std::string, std::vector<const WhereaboutsMember*>> byRoom;
  std::map<std::string, WhereaboutsCertainty> weakest;
  const std::vector<WhereaboutsMember> members = activeMembers(facts);
  Json::Value people(Json::arrayValue);
  for (const WhereaboutsMember& member : members) {
    if (member.userId == ask.askerId)
      continue;
    const RoomVerdict* verdict = verdictFor(facts, member.userId);
    if (verdict == nullptr || roleOfRoom(verdict->room) != *role)
      continue;
    byRoom[verdict->room].push_back(&member);
    const WhereaboutsCertainty certainty =
        certaintyOfVerdict({.verdict = *verdict, .facts = facts, .english = english});
    const auto current = weakest.find(verdict->room);
    if (current == weakest.end() || certainty > current->second)
      weakest[verdict->room] = certainty;
    people.append(entryOf({.member = member, .facts = facts, .english = english}));
  }
  WhereaboutsVoiceReply reply{.ok = true, .text = {}, .code = {}, .structured = Json::Value(Json::objectValue)};
  reply.structured["room"] = *role;
  if (byRoom.empty()) {
    const std::string place =
        whereabouts::roomLabel({.room = "0:" + *role, .english = english, .facts = facts});
    reply.text = pick(english, "No detecto a nadie de casa " + place + ".",
                      "I do not detect anyone from home " + place + ".");
  }
  for (const auto& [room, found] : byRoom) {
    std::vector<std::string> names;
    for (const WhereaboutsMember* member : found)
      names.push_back(member->name);
    const std::string place = whereabouts::roomLabel({.room = room, .english = english, .facts = facts});
    if (!reply.text.empty())
      reply.text += ' ';
    if (english)
      reply.text += capitalized(place) + (names.size() == 1 ? " is " : " are ") + joinNames(names, true) + ". ";
    else
      reply.text += capitalized(place) + (names.size() == 1 ? " está " : " están ") + joinNames(names, false) + ". ";
    reply.text += certaintyWords(weakest.at(room), english) + ".";
  }
  if (someoneHidden(facts, ask.askerId))
    reply.text += pick(english, " Solo cuento a quien comparte su ubicación.",
                       " I only count those who share their location.");
  reply.structured["people"] = std::move(people);
  return reply;
}

WhereaboutsVoiceReply householdAnswer(const WhereaboutsAsk& ask, const WhereaboutsFacts& facts)
{
  const bool english = ask.lang == "en";
  std::vector<std::string> home;
  std::vector<std::string> away;
  std::vector<std::string> unknown;
  Json::Value people(Json::arrayValue);
  for (const WhereaboutsMember& member : activeMembers(facts)) {
    if (member.userId == ask.askerId)
      continue;
    people.append(entryOf({.member = member, .facts = facts, .english = english}));
    if (const RoomVerdict* verdict = verdictFor(facts, member.userId)) {
      home.push_back(member.name + " (" +
                     whereabouts::roomLabel({.room = verdict->room, .english = english, .facts = facts}) + ")");
      continue;
    }
    const WhereaboutsPresence* presence = presenceFor(facts, member.userId);
    if (presence != nullptr && presence->state == PresenceState::Home)
      home.push_back(member.name);
    else if (presence != nullptr && presence->state == PresenceState::Away)
      away.push_back(member.name);
    else
      unknown.push_back(member.name);
  }
  WhereaboutsVoiceReply reply{.ok = true, .text = {}, .code = {}, .structured = Json::Value(Json::objectValue)};
  if (!home.empty())
    reply.text += pick(english, "En casa: ", "Home: ") + joinNames(home, english) + ".";
  if (!away.empty())
    reply.text += (reply.text.empty() ? "" : " ") + pick(english, "Fuera: ", "Away: ") + joinNames(away, english) + ".";
  if (!unknown.empty())
    reply.text += (reply.text.empty() ? "" : " ") + pick(english, "No sé dónde están: ", "I do not know where they are: ") +
                  joinNames(unknown, english) + ".";
  if (reply.text.empty())
    reply.text = pick(english, "No hay nadie más en casa registrado en Argus.", "There is nobody else at home registered in Argus.");
  reply.structured["people"] = std::move(people);
  return reply;
}
}

namespace whereabouts
{

std::optional<std::string> roomRoleOf(std::string_view spoken)
{
  std::string folded = text_norm::folded(std::string(spoken));
  for (const std::string_view article : {"la ", "el ", "los ", "las ", "the ", "mi ", "my ", "our ", "nuestra ", "nuestro "})
    if (folded.starts_with(article)) {
      folded.erase(0, article.size());
      break;
    }
  const auto found = std::ranges::find(kRoomSynonyms, std::string_view(folded), &RoomSynonym::spoken);
  if (found != kRoomSynonyms.end())
    return std::string(found->role);
  const auto role = std::ranges::find(kRoomLabels, std::string_view(folded), &RoomWords::role);
  if (role != kRoomLabels.end())
    return std::string(role->role);
  return std::nullopt;
}

std::string roomLabel(const WhereaboutsRoomLabelInput& input)
{
  const RoomKey key = keyOf(input.room);
  std::string label;
  if (key.cameraId > 0) {
    const auto camera = std::ranges::find(input.facts.cameras, key.cameraId, &WhereaboutsCamera::id);
    const std::string name =
        camera == input.facts.cameras.end() || camera->name.empty() ? std::to_string(key.cameraId) : camera->name;
    label = pick(input.english, "cerca de la cámara " + name, "near the " + name + " camera");
  }
  else {
    const auto words = std::ranges::find(kRoomLabels, std::string_view(key.role), &RoomWords::role);
    label = words == kRoomLabels.end() ? pick(input.english, "en " + key.role, "in " + key.role)
                                       : std::string(input.english ? words->en : words->es);
  }
  if (input.facts.places.size() > 1 && key.environmentId > 0) {
    const auto place = std::ranges::find(input.facts.places, key.environmentId, &WhereaboutsPlace::id);
    if (place != input.facts.places.end() && !place->name.empty())
      label += pick(input.english, " de " + place->name, " at " + place->name);
  }
  return label;
}

WhereaboutsCertainty certaintyOf(const WhereaboutsCertaintyInput& input)
{
  WhereaboutsCertainty certainty = input.confidence >= kSureShare  ? WhereaboutsCertainty::Sure
                                   : input.confidence >= kFairShare ? WhereaboutsCertainty::Fairly
                                                                    : WhereaboutsCertainty::Unsure;
  if (input.ageMs > kStaleMs && certainty != WhereaboutsCertainty::Unsure)
    certainty = certainty == WhereaboutsCertainty::Sure ? WhereaboutsCertainty::Fairly : WhereaboutsCertainty::Unsure;
  return certainty;
}

WhereaboutsVoiceReply answer(const WhereaboutsAnswerInput& input)
{
  if (!text_norm::folded(input.ask.person).empty())
    return personAnswer(input.ask, input.facts);
  if (!text_norm::folded(input.ask.room).empty())
    return roomAnswer(input.ask, input.facts);
  return householdAnswer(input.ask, input.facts);
}

}

WhereaboutsService::WhereaboutsService(WhereaboutsDependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

drogon::Task<std::optional<WhereaboutsFacts>> WhereaboutsService::gather() const
{
  WhereaboutsFacts facts;
  if (!dependencies_.members)
    co_return std::nullopt;
  const auto members = dependencies_.members;
  auto listed = co_await BlockingTask<std::optional<std::vector<WhereaboutsMember>>>([members]() { return members(); });
  if (!listed)
    co_return std::nullopt;
  facts.members = std::move(*listed);
  if (dependencies_.verdicts)
    facts.verdicts = co_await dependencies_.verdicts();
  if (dependencies_.located)
    facts.located = co_await dependencies_.located();
  if (!facts.located)
    facts.verdicts.clear();
  else
    std::erase_if(facts.verdicts,
                  [&facts](const RoomVerdict& verdict) { return !facts.located->contains(verdict.userId); });
  if (dependencies_.presence)
    facts.presence = co_await dependencies_.presence();
  if (dependencies_.places)
    facts.places = co_await dependencies_.places();
  for (const RoomVerdict& verdict : facts.verdicts) {
    const RoomKey key = keyOf(verdict.room);
    if (key.cameraId <= 0 || std::ranges::find(facts.cameras, key.cameraId, &WhereaboutsCamera::id) != facts.cameras.end())
      continue;
    facts.cameras.push_back(
        {.id = key.cameraId, .name = dependencies_.cameraName ? dependencies_.cameraName(key.cameraId) : std::string()});
  }
  facts.nowMs = dependencies_.clockMs ? dependencies_.clockMs()
                                      : std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
  co_return facts;
}

drogon::Task<WhereaboutsVoiceReply> WhereaboutsService::answer(WhereaboutsVoiceRequest request) const
{
  const bool english = request.lang == "en";
  if (parseUserRole(request.role).value_or(UserRole::Unknown) != UserRole::Owner)
    co_return failure(pick(english, "Solo el dueño de casa puede preguntar dónde está alguien.",
                           "Only the owner can ask where someone is."),
                      "forbidden");
  const std::optional<WhereaboutsFacts> facts = co_await gather();
  if (!facts)
    co_return failure(pick(english, "Ahora no puedo saber dónde está nadie.", "I cannot tell where anyone is right now."),
                      "whereabouts_unavailable");
  const WhereaboutsAsk ask{.askerId = request.userId,
                           .lang = request.lang,
                           .person = request.person,
                           .room = request.room};
  co_return whereabouts::answer({.ask = ask, .facts = *facts});
}
