#include "guard-copy.hxx"

#include <algorithm>
#include <array>
#include <utility>
#include <cctype>
#include <ranges>

namespace
{

struct Phrase
{
  std::string_view es;
  std::string_view en;
};

std::string pick(const Phrase& phrase, bool english)
{
  return std::string(english ? phrase.en : phrase.es);
}

std::string capitalized(std::string text)
{
  if (!text.empty())
    text.front() = static_cast<char>(
        std::toupper(static_cast<unsigned char>(text.front())));
  return text;
}

std::string joined(const std::vector<std::string>& parts,
                   std::string_view separator)
{
  std::string text;
  for (const auto& part : parts) {
    if (part.empty())
      continue;
    if (!text.empty())
      text += separator;
    text += part;
  }
  return text;
}

std::string cameraLabel(const GuardNotice& notice, bool english)
{
  std::string label =
      notice.cameraName.empty()
          ? (english ? "Camera " : "Cámara ") + std::to_string(notice.cameraId)
          : notice.cameraName;
  if (!notice.environmentName.empty())
    label += " (" + notice.environmentName + ")";
  return label;
}

std::string subjectPhrase(const GuardNotice& notice, bool english)
{
  switch (notice.subject) {
    case NoticeSubject::Stranger:
      return pick({.es = "Persona desconocida", .en = "Unknown person"},
                  english);
    case NoticeSubject::Unobserved:
      return pick({.es = "Alguien sin identificar", .en = "Someone unidentified"},
                  english);
    case NoticeSubject::Several:
      return std::to_string(std::max(2, notice.people)) +
             (english ? " unknown people" : " personas desconocidas");
    case NoticeSubject::Accompanied:
      return pick({.es = "Persona desconocida acompañada",
                   .en = "Unknown person with company"},
                  english);
  }
  return pick({.es = "Persona desconocida", .en = "Unknown person"}, english);
}

std::string placePhrase(const GuardNotice& notice, bool english)
{
  switch (notice.role) {
    case CameraRole::Entrance:
      return pick({.es = "en la entrada", .en = "at the entrance"}, english);
    case CameraRole::Perimeter:
      return pick({.es = "en el exterior", .en = "outside"}, english);
    case CameraRole::Garage:
      return pick({.es = "en el garaje", .en = "in the garage"}, english);
    case CameraRole::Living:
      return pick({.es = "en la zona de estar", .en = "in the living area"},
                  english);
    case CameraRole::Kitchen:
      return pick({.es = "en la cocina", .en = "in the kitchen"}, english);
    case CameraRole::Office:
      return pick({.es = "en la oficina", .en = "in the office"}, english);
    case CameraRole::Register:
      return pick({.es = "en la caja", .en = "at the register"}, english);
    case CameraRole::Storage:
      return pick({.es = "en el almacén", .en = "in the storeroom"}, english);
    case CameraRole::PublicArea:
      return pick({.es = "en la zona de público", .en = "in the public area"},
                  english);
    case CameraRole::Other:
      return notice.outdoor ? pick({.es = "fuera", .en = "outside"}, english)
                            : std::string{};
  }
  return {};
}

std::string reasonPhrase(GuardReason reason, const GuardNotice& notice,
                         bool english)
{
  switch (reason) {
    case GuardReason::AfterHours:
      return pick({.es = "fuera de horario", .en = "after hours"}, english);
    case GuardReason::NobodyHome:
      return pick({.es = "sin nadie dentro", .en = "while nobody is in"},
                  english);
    case GuardReason::Armed:
      return pick({.es = "con la vigilancia armada", .en = "while armed"},
                  english);
    case GuardReason::Night:
      return pick({.es = "de noche", .en = "at night"}, english);
    case GuardReason::AlertZone:
      if (!notice.zoneName.empty())
        return english ? "in “" + notice.zoneName + "”"
                       : "en «" + notice.zoneName + "»";
      return pick({.es = "en la zona de alerta", .en = "in the alert zone"},
                  english);
    case GuardReason::RepeatVisits:
      return pick({.es = "ha pasado varias veces hoy",
                   .en = "seen several times today"},
                  english);
    case GuardReason::Escalating:
      return pick({.es = "cada vez más actividad", .en = "activity is building"},
                  english);
    case GuardReason::FaceHidden:
      return notice.subject == NoticeSubject::Unobserved
                 ? std::string{}
                 : pick({.es = "sin verle la cara", .en = "face not visible"},
                        english);
    case GuardReason::Lingering:
      return notice.dwellS >= 5
                 ? std::string{}
                 : pick({.es = "se ha quedado un rato", .en = "has stayed a while"},
                        english);
    default:
      return {};
  }
}

std::string durationPhrase(int64_t seconds, bool english)
{
  const std::string amount =
      seconds < 90 ? std::to_string(seconds) + (english ? "s" : " s")
                   : std::to_string((seconds + 30) / 60) + " min";
  return (english ? "for " : "desde hace ") + amount;
}

std::string dangerWord(GuardDanger danger, bool english)
{
  switch (danger) {
    case GuardDanger::Critical:
      return pick({.es = "crítico", .en = "critical"}, english);
    case GuardDanger::High:
      return pick({.es = "alto", .en = "high"}, english);
    case GuardDanger::Medium:
      return pick({.es = "medio", .en = "medium"}, english);
    default:
      return pick({.es = "bajo", .en = "low"}, english);
  }
}

std::string actionSentence(NoticeAction action, bool english)
{
  switch (action) {
    case NoticeAction::Speaker:
      return pick({.es = "Argus le está avisando por el altavoz.",
                   .en = "Argus is warning them over the speaker."},
                  english);
    case NoticeAction::Alarm:
      return pick({.es = "La alarma de la cámara está sonando.",
                   .en = "The camera alarm is sounding."},
                  english);
    case NoticeAction::Greeted:
      return pick({.es = "Argus le ha saludado y ha respondido.",
                   .en = "Argus greeted them and they answered."},
                  english);
    case NoticeAction::GreetedNoReply:
      return pick({.es = "Argus le ha saludado; no ha respondido.",
                   .en = "Argus greeted them; no answer."},
                  english);
    case NoticeAction::SilentWeapon:
      return pick({.es = "Argus se mantiene en silencio para no poner a nadie "
                         "en riesgo.",
                   .en = "Argus stays silent so no one is put at risk."},
                  english);
    case NoticeAction::Watching:
      return pick({.es = "Argus sigue observando.", .en = "Argus keeps watching."},
                  english);
  }
  return {};
}

bool hasReason(const GuardNotice& notice, GuardReason reason)
{
  return std::ranges::find(notice.reasons, reason) != notice.reasons.end();
}

std::string situationSentence(const GuardNotice& notice, bool english)
{
  std::vector<std::string> parts{placePhrase(notice, english)};
  if (!notice.zoneName.empty() && !hasReason(notice, GuardReason::AlertZone))
    parts.push_back(english ? "in \u201c" + notice.zoneName + "\u201d"
                            : "en \u00ab" + notice.zoneName + "\u00bb");
  int picked = 0;
  for (const GuardReason reason : notice.reasons) {
    if (picked >= 2 || !guardReasonRaises(reason) ||
        reason == GuardReason::Weapon)
      continue;
    std::string phrase = reasonPhrase(reason, notice, english);
    if (phrase.empty())
      continue;
    parts.push_back(std::move(phrase));
    ++picked;
  }
  if (notice.dwellS >= 5)
    parts.push_back(durationPhrase(notice.dwellS, english));
  const std::string text = joined(parts, ", ");
  return text.empty() ? std::string{} : capitalized(text) + ".";
}

std::string categoryNoun(std::string_view category, bool english)
{
  constexpr std::array<std::pair<std::string_view, Phrase>, 6> kNouns{{
      {"neighbor", {.es = "el vecino", .en = "the neighbour"}},
      {"delivery", {.es = "el repartidor", .en = "the courier"}},
      {"service", {.es = "el técnico de servicio", .en = "the service worker"}},
      {"family", {.es = "un familiar", .en = "a relative"}},
      {"acquaintance", {.es = "un conocido", .en = "an acquaintance"}},
      {"watchlist", {.es = "alguien de tu lista de vigilancia",
                     .en = "someone on your watchlist"}},
  }};
  for (const auto& [key, phrase] : kNouns)
    if (key == category)
      return pick(phrase, english);
  return {};
}

std::string weekdayPlural(int day, bool english)
{
  constexpr std::array<Phrase, 7> kDays{{{.es = "domingos", .en = "Sundays"},
                                         {.es = "lunes", .en = "Mondays"},
                                         {.es = "martes", .en = "Tuesdays"},
                                         {.es = "miércoles", .en = "Wednesdays"},
                                         {.es = "jueves", .en = "Thursdays"},
                                         {.es = "viernes", .en = "Fridays"},
                                         {.es = "sábados", .en = "Saturdays"}}};
  if (day < 0 || day > 6)
    return {};
  return pick(kDays.at(static_cast<std::size_t>(day)), english);
}

std::string habitPhrase(const GuardVisitor& visitor, bool english)
{
  std::vector<std::string> days;
  days.reserve(visitor.weekdays.size());
  for (const int day : visitor.weekdays)
    days.push_back(weekdayPlural(day, english));
  std::string habit;
  if (!days.empty())
    habit = (english ? "who usually comes on " : "que suele venir los ") +
            joined(days, english ? " and " : " y ");
  if (visitor.usualHour >= 0) {
    const std::string hour = std::to_string(visitor.usualHour) + ":00";
    habit += habit.empty()
                 ? (english ? "who usually comes around " : "que suele venir hacia las ") + hour
                 : (english ? " around " : " hacia las ") + hour;
  }
  return habit;
}

std::string visitorSentenceOf(const GuardVisitor& visitor, bool english)
{
  if (!visitor.present())
    return {};
  const std::string noun = categoryNoun(visitor.category, english);
  const std::string habit = habitPhrase(visitor, english);
  std::string who;
  if (!visitor.name.empty())
    who = noun.empty() ? visitor.name : visitor.name + ", " + noun;
  else
    who = noun;
  if (who.empty())
    return {};
  std::string sentence = visitor.companion
                             ? (english ? "With " : "Con ") + who
                             : (english ? "It is " : "Es ") + who;
  if (!habit.empty() && visitor.category != "watchlist")
    sentence += (visitor.name.empty() ? " " : ", ") + habit;
  return sentence + ".";
}

NoticeText renderEpisode(const GuardNotice& notice, bool english)
{
  const std::string camera = cameraLabel(notice, english);
  NoticeText text;
  if (notice.kind == NoticeKind::Escalation)
    text.title = english ? "Still at " + camera + " · " +
                               dangerWord(notice.danger, english) + " risk"
                         : "Sigue en " + camera + " · riesgo " +
                               dangerWord(notice.danger, english);
  else if (notice.visitor.category == "watchlist" && !notice.visitor.companion)
    text.title = (notice.visitor.name.empty()
                      ? pick({.es = "Persona en vigilancia", .en = "Watchlist person"}, english)
                      : notice.visitor.name) +
                 " · " + camera;
  else
    text.title = subjectPhrase(notice, english) + " · " + camera;
  std::vector<std::string> sentences;
  sentences.push_back(visitorSentenceOf(notice.visitor, english));
  if (hasReason(notice, GuardReason::Weapon))
    sentences.push_back(pick({.es = "Posible arma a la vista.",
                              .en = "Possible weapon in view."},
                             english));
  sentences.push_back(situationSentence(notice, english));
  sentences.push_back(actionSentence(notice.action, english));
  text.body = joined(sentences, " ");
  return text;
}

NoticeText renderTamper(const GuardNotice& notice, bool english)
{
  const std::string camera = cameraLabel(notice, english);
  const std::string since = durationPhrase(std::max<int64_t>(notice.dwellS, 60),
                                           english);
  NoticeText text;
  text.title = (english ? "Check camera " : "Revisa la cámara ") + camera;
  if (notice.tamperStatus == "covered")
    text.body = english ? "The view has been blocked " + since +
                              ". Argus can't see that area."
                        : "La imagen está tapada " + since +
                              ". Argus no ve esa zona.";
  else if (notice.tamperStatus == "moved")
    text.body = pick({.es = "Parece que la han movido o girado. Argus ya no "
                            "vigila la misma zona.",
                      .en = "It seems to have been moved or turned. Argus is "
                            "no longer watching the same area."},
                     english);
  else if (notice.tamperStatus == "blurred")
    text.body = english ? "The image has been blurry " + since + "."
                        : "La imagen está borrosa " + since + ".";
  else if (notice.tamperStatus == "unreachable")
    text.body = english ? "Argus has had no picture from it " + since +
                              ": it may be unplugged, off the network or "
                              "switched off. That area is not being watched."
                        : "Argus no recibe imagen " + since +
                              ": puede estar desenchufada, sin red o "
                              "apagada. Esa zona no está vigilada.";
  else
    text.body = english ? "The image has been unreliable " + since + "."
                        : "La imagen no es fiable " + since + ".";
  if (notice.danger == GuardDanger::Critical)
    text.body += pick({.es = " La vigilancia está activa: revísalo cuanto antes.",
                       .en = " Guarding is on: check it as soon as you can."},
                      english);
  return text;
}

NoticeText renderSafety(const GuardNotice& notice, bool english)
{
  const std::string who = notice.actorName.empty()
                              ? pick({.es = "Alguien de casa", .en = "Someone at home"}, english)
                              : notice.actorName;
  const std::string place =
      notice.environmentName.empty() ? std::string{} : " (" + notice.environmentName + ")";
  NoticeText text;
  if (notice.kind == NoticeKind::Panic) {
    text.title = (english ? "Panic button · " : "Botón de pánico · ") + who + place;
    text.body = who + (english ? " asked for help silently from the app. Check carefully: a "
                                 "call or a message could give them away."
                               : " ha pedido ayuda en silencio desde la app. Compruébalo con "
                                 "cuidado: una llamada o un mensaje podrían delatarle.");
    return text;
  }
  text.title = (english ? "Silent alert · " : "Alerta silenciosa · ") + who + place;
  text.body = who + (english ? " turned guarding off with their duress code: someone may be "
                               "forcing them. Don't call them; check carefully or get help."
                             : " ha desactivado la vigilancia con su código de coacción: puede "
                               "que alguien le obligue. No le llames; compruébalo con cuidado "
                               "o pide ayuda.");
  return text;
}

std::string digestList(const std::vector<DigestLine>& lines, bool english)
{
  std::vector<DigestLine> ordered = lines;
  std::ranges::stable_sort(ordered, std::ranges::greater{}, &DigestLine::count);
  std::vector<std::string> parts;
  int64_t rest = 0;
  for (size_t index = 0; index < ordered.size(); ++index) {
    const DigestLine& line = ordered[index];
    if (index >= 3) {
      ++rest;
      continue;
    }
    const std::string name =
        line.cameraName.empty()
            ? (english ? "Camera " : "Cámara ") + std::to_string(line.cameraId)
            : line.cameraName;
    parts.push_back(name + " " + std::to_string(line.count));
  }
  std::string text = joined(parts, ", ");
  if (rest > 0)
    text += (english ? " and " : " y ") + std::to_string(rest) +
            (english ? " more" : " más");
  return text;
}

int64_t total(const std::vector<DigestLine>& lines)
{
  int64_t sum = 0;
  for (const auto& line : lines)
    sum += line.count;
  return sum;
}

NoticeText renderDigest(const GuardNotice& notice, bool english)
{
  NoticeText text;
  text.title = notice.afterQuiet
                   ? pick({.es = "Mientras descansabas",
                           .en = "While you were resting"},
                          english)
                   : pick({.es = "Resumen de vigilancia",
                           .en = "Security summary"},
                          english);
  if (!notice.environmentName.empty())
    text.title += " · " + notice.environmentName;
  std::vector<std::string> sentences;
  const int64_t held = total(notice.held);
  if (held > 0)
    sentences.push_back(
        english ? "Argus held " + std::to_string(held) +
                      (held == 1 ? " alert: " : " alerts: ") +
                      digestList(notice.held, english) + "."
                : "Argus guardó " + std::to_string(held) +
                      (held == 1 ? " aviso: " : " avisos: ") +
                      digestList(notice.held, english) + ".");
  else if (notice.notified == 0)
    sentences.push_back(pick({.es = "Nada requirió tu atención.",
                              .en = "Nothing needed you."},
                             english));
  if (!notice.routine.empty())
    sentences.push_back((english ? "Routine activity: " : "Actividad normal: ") +
                        digestList(notice.routine, english) + ".");
  text.body = joined(sentences, " ");
  return text;
}

}

std::string noticeKindToString(NoticeKind kind)
{
  switch (kind) {
    case NoticeKind::Episode:
    case NoticeKind::Escalation:
      return "guard_episode";
    case NoticeKind::Tamper:
      return "guard_tamper";
    case NoticeKind::Digest:
      return "guard_digest";
    case NoticeKind::Panic:
      return "guard_panic";
    case NoticeKind::Duress:
      return "guard_duress";
  }
  return "guard_episode";
}

std::string noticeSubjectToString(NoticeSubject subject)
{
  switch (subject) {
    case NoticeSubject::Stranger:
      return "stranger";
    case NoticeSubject::Unobserved:
      return "unobserved";
    case NoticeSubject::Several:
      return "several";
    case NoticeSubject::Accompanied:
      return "accompanied";
  }
  return "stranger";
}

std::string noticeActionToString(NoticeAction action)
{
  switch (action) {
    case NoticeAction::Watching:
      return "watching";
    case NoticeAction::Speaker:
      return "speaker";
    case NoticeAction::Alarm:
      return "alarm";
    case NoticeAction::Greeted:
      return "greeted";
    case NoticeAction::GreetedNoReply:
      return "greeted_no_reply";
    case NoticeAction::SilentWeapon:
      return "silent_weapon";
  }
  return "watching";
}

std::string guard_copy::normalizeLang(const LangPreference& preference)
{
  const auto prefix = [](std::string_view value) {
    std::string code;
    for (const char step : value.substr(0, 2))
      code.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(step))));
    return code;
  };
  std::string code = prefix(preference.requested);
  if (code == "es" || code == "en")
    return code;
  return prefix(preference.fallback) == "en" ? "en" : "es";
}

NoticeText guard_copy::render(const GuardNotice& notice, std::string_view lang)
{
  const bool english =
      normalizeLang({.requested = lang, .fallback = "es"}) == "en";
  switch (notice.kind) {
    case NoticeKind::Episode:
    case NoticeKind::Escalation:
      return renderEpisode(notice, english);
    case NoticeKind::Tamper:
      return renderTamper(notice, english);
    case NoticeKind::Digest:
      return renderDigest(notice, english);
    case NoticeKind::Panic:
    case NoticeKind::Duress:
      return renderSafety(notice, english);
  }
  return renderEpisode(notice, english);
}

NoticeText guard_copy::unattended(std::string_view lang)
{
  const bool english =
      normalizeLang({.requested = lang, .fallback = "es"}) == "en";
  if (english)
    return {.title = "Nobody else to warn",
            .body = "There is nobody else to warn. Add emergency contacts in Security."};
  return {.title = "Nadie más a quien avisar",
          .body = "No hay nadie más a quién avisar. Configura contactos de emergencia en Vigilancia."};
}

NoticeText guard_copy::panicContacts(const PanicContactsInput& input)
{
  NoticeText text = render(input.notice, input.lang);
  std::string list;
  for (const GuardContact& contact : input.contacts) {
    if (contact.name.empty() && contact.phone.empty())
      continue;
    if (!list.empty())
      list += ", ";
    list += contact.name;
    if (!contact.phone.empty())
      list += " (" + contact.phone + ")";
  }
  if (list.empty())
    return text;
  const bool english =
      normalizeLang({.requested = input.lang, .fallback = "es"}) == "en";
  text.body = english
                  ? "There is nobody else to warn. Call your emergency contacts: " +
                        list + "."
                  : "No hay nadie más a quién avisar. Llama a tus contactos de "
                    "emergencia: " + list + ".";
  return text;
}

std::string guard_copy::urgency(const GuardNotice& notice)
{
  switch (notice.kind) {
    case NoticeKind::Digest:
      return "passive";
    case NoticeKind::Tamper:
      return notice.danger == GuardDanger::Critical ? "critical" : "active";
    case NoticeKind::Panic:
    case NoticeKind::Duress:
      return "critical";
    case NoticeKind::Episode:
    case NoticeKind::Escalation:
      break;
  }
  if (notice.danger == GuardDanger::Critical)
    return "critical";
  if (notice.danger == GuardDanger::High)
    return "time_sensitive";
  return "active";
}

std::string guard_copy::environmentDefaultName(EnvironmentKind kind,
                                               std::string_view lang)
{
  const bool english =
      normalizeLang({.requested = lang, .fallback = "es"}) == "en";
  switch (kind) {
    case EnvironmentKind::Home:
      return pick({.es = "Casa", .en = "Home"}, english);
    case EnvironmentKind::Office:
      return pick({.es = "Oficina", .en = "Office"}, english);
    case EnvironmentKind::Commercial:
      return pick({.es = "Local", .en = "Shop"}, english);
    case EnvironmentKind::Restaurant:
      return pick({.es = "Restaurante", .en = "Restaurant"}, english);
    case EnvironmentKind::Warehouse:
      return pick({.es = "Almacén", .en = "Warehouse"}, english);
    case EnvironmentKind::Outdoor:
      return pick({.es = "Exterior", .en = "Outdoors"}, english);
  }
  return pick({.es = "Casa", .en = "Home"}, english);
}

std::string guard_copy::visitorSentence(const GuardVisitor& visitor, std::string_view lang)
{
  return visitorSentenceOf(visitor, normalizeLang({.requested = lang, .fallback = "es"}) == "en");
}
