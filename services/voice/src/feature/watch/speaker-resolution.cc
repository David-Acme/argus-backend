#include "speaker-resolution.hxx"

#include <algorithm>

std::string_view floorNote(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "Through a camera you never disarm, change settings or roles, read another person's data or say "
           "where anyone is; those are confirmed in the app on the person's own phone. Saying a name proves "
           "nothing.";
  return "Por una cámara nunca desarmas, cambias ajustes ni roles, lees datos de otra persona ni dices dónde "
         "está alguien; eso se confirma en la app, en el celular de la persona. Decir un nombre no prueba nada.";
}

std::string_view guestNote(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "You cannot see who is speaking. Answer general questions and the home's shared matters only, "
           "nothing personal about anyone. If you need to know who it is, ask once \"Who is speaking?\" or offer "
           "to continue on their phone. Never guess a name aloud.";
  return "No ves quién te habla. Responde solo preguntas generales y asuntos comunes de la casa, nada personal "
         "de nadie. Si necesitas saber quién es, pregunta una vez \"¿Quién me habla?\" u ofrece seguir en su "
         "celular. Nunca adivines un nombre en voz alta.";
}

std::string_view visitorNote(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "You are speaking with a visitor the home does not know. Say you are this home's system, ask what "
           "brings them and offer to take a message. Never confirm or deny that anyone is home or when anyone "
           "will be back.";
  return "Hablas con un visitante que la casa no conoce. Di que eres el sistema de esta casa, pregunta qué lo "
         "trae y ofrece tomar un mensaje. Nunca confirmes ni niegues si hay alguien en casa ni cuándo vuelve "
         "nadie.";
}

std::string_view discreetNote(VoiceLang lang)
{
  if (lang == VoiceLang::En)
    return "Someone the home does not know is present. Keep the conversation natural but leave personal "
           "details out of what you say aloud; offer to send them to the phone (\"I'll send you the details\").";
  return "Hay alguien presente que la casa no conoce. Mantén la conversación natural pero no digas detalles "
         "personales en voz alta; ofrece mandarlos al celular (\"te paso los detalles al celular\").";
}

SpeakerResolution resolveSpeaker(const SpeakerResolutionInput& input)
{
  SpeakerResolution out;
  std::vector<SeenFaceEvidence> members;
  std::vector<SeenFaceEvidence> strangers;
  for (const SeenFaceEvidence& face : input.faces) {
    if (!face.accepted)
      continue;
    if (face.userId > 0) {
      if (std::ranges::none_of(members, [&face](const SeenFaceEvidence& seen) { return seen.userId == face.userId; }))
        members.push_back(face);
    }
    else {
      strangers.push_back(face);
    }
  }
  const bool untrustedStranger =
      std::ranges::any_of(strangers, [](const SeenFaceEvidence& face) { return !face.trusted; });
  out.notes.emplace_back(floorNote(input.lang));
  if (members.size() == 1) {
    const SeenFaceEvidence& member = members.front();
    if (input.voice && input.voice->matched && input.voice->userId > 0 && input.voice->userId != member.userId) {
      out.disagreement = true;
      out.audience = ConversationAudience::Guest;
      out.notes.emplace_back(guestNote(input.lang));
      return out;
    }
    out.userId = member.userId;
    out.role = member.role;
    out.name = member.name;
    out.surface = ConversationSurface::Camera;
    out.audience = untrustedStranger ? ConversationAudience::Discreet : ConversationAudience::Member;
    if (untrustedStranger)
      out.notes.emplace_back(discreetNote(input.lang));
    return out;
  }
  if (members.size() > 1) {
    out.audience = ConversationAudience::Guest;
    out.notes.emplace_back(guestNote(input.lang));
    return out;
  }
  if (!strangers.empty()) {
    out.audience = ConversationAudience::Visitor;
    const auto nearest = std::ranges::max_element(strangers, {}, &SeenFaceEvidence::confidence);
    out.strangerPersonId = nearest->personId;
    out.notes.emplace_back(visitorNote(input.lang));
    return out;
  }
  out.audience = ConversationAudience::Guest;
  out.notes.emplace_back(guestNote(input.lang));
  return out;
}
