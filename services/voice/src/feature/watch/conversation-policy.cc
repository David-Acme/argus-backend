#include "conversation-policy.hxx"

#include <chrono>
#include <random>

std::string_view conversationStateToString(ConversationState state)
{
  switch (state) {
    case ConversationState::Waking:
      return "waking";
    case ConversationState::Listening:
      return "listening";
    case ConversationState::Thinking:
      return "thinking";
    case ConversationState::Speaking:
      return "speaking";
    case ConversationState::Resumable:
      return "resumable";
    case ConversationState::Ended:
      return "ended";
  }
  return "ended";
}

std::string_view conversationHandoffToString(ConversationHandoff handoff)
{
  switch (handoff) {
    case ConversationHandoff::Camera:
      return "camera";
    case ConversationHandoff::Offering:
      return "offering";
    case ConversationHandoff::Bridging:
      return "bridging";
    case ConversationHandoff::Device:
      return "device";
  }
  return "camera";
}

std::string_view conversationAudienceToString(ConversationAudience audience)
{
  switch (audience) {
    case ConversationAudience::Member:
      return "member";
    case ConversationAudience::Discreet:
      return "discreet";
    case ConversationAudience::Guest:
      return "guest";
    case ConversationAudience::Visitor:
      return "visitor";
  }
  return "guest";
}

std::string_view conversationEndToString(ConversationEnd end)
{
  switch (end) {
    case ConversationEnd::None:
      return "";
    case ConversationEnd::Silence:
      return "silence";
    case ConversationEnd::Closing:
      return "closing";
    case ConversationEnd::Limit:
      return "limit";
    case ConversationEnd::CameraLost:
      return "camera_lost";
    case ConversationEnd::BusyLine:
      return "busy";
    case ConversationEnd::Handoff:
      return "handoff";
    case ConversationEnd::Superseded:
      return "superseded";
    case ConversationEnd::Shutdown:
      return "shutdown";
    case ConversationEnd::Gate:
      return "gate";
    case ConversationEnd::Failed:
      return "failed";
    case ConversationEnd::Expired:
      return "expired";
  }
  return "";
}

std::string_view visitorConsentToString(VisitorConsent consent)
{
  switch (consent) {
    case VisitorConsent::None:
      return "none";
    case VisitorConsent::Asked:
      return "unanswered";
    case VisitorConsent::Granted:
      return "granted";
    case VisitorConsent::Declined:
      return "declined";
  }
  return "none";
}

ConversationEnd conversationDue(const ConversationClockInput& input)
{
  if (input.now - input.startedAt >= input.maxLengthMs)
    return ConversationEnd::Limit;
  if (!input.busy && input.now - input.lastSpeech >= input.idleCloseMs)
    return ConversationEnd::Silence;
  return ConversationEnd::None;
}

ReplyNudge replyNudge(const ReplyTimerInput& input)
{
  if (input.userTurnAt <= 0 || input.firstAudioAt >= input.userTurnAt)
    return ReplyNudge::None;
  const int64_t waited = input.now - input.userTurnAt;
  if (waited >= input.failAfterMs)
    return ReplyNudge::GiveUp;
  if (!input.acked && waited >= input.ackAfterMs)
    return ReplyNudge::Acknowledge;
  return ReplyNudge::None;
}

std::string_view acknowledgementLine(VoiceLang lang)
{
  return lang == VoiceLang::En ? "One moment." : "Un momento.";
}

std::string_view giveUpLine(VoiceLang lang)
{
  return lang == VoiceLang::En ? "I couldn't answer right now; try again or continue in the app."
                               : "No pude responder ahora; inténtalo de nuevo o sigue en la app.";
}

std::string cameraNote(VoiceLang lang, std::string_view cameraName)
{
  if (lang == VoiceLang::En)
    return "You are speaking through the speaker of the " + std::string(cameraName) +
           " camera. Keep answers short, never read long lists aloud, and never say your own name.";
  return "Estás hablando por el altavoz de la cámara " + std::string(cameraName) +
         ". Responde breve, no leas listas largas en voz alta y no digas tu propio nombre.";
}

std::string busyNotice(VoiceLang lang, std::string_view cameraName)
{
  if (lang == VoiceLang::En)
    return "I heard you on the " + std::string(cameraName) +
           " camera, but someone is using it; continue in the app.";
  return "Te escuché en la cámara " + std::string(cameraName) +
         ", pero alguien la está usando; sigue en la app.";
}

std::string_view handoffLine(VoiceLang lang)
{
  return lang == VoiceLang::En ? "I'll pass it to your phone." : "Te lo paso al celular.";
}

std::string_view continueLine(VoiceLang lang)
{
  return lang == VoiceLang::En ? "We're continuing here." : "Seguimos aquí.";
}

std::string mintConversationId()
{
  constexpr std::string_view kHex = "0123456789abcdef";
  std::random_device entropy;
  std::string id;
  id.reserve(32);
  while (id.size() < 32) {
    const unsigned word = entropy();
    for (unsigned shift = 0; shift < 32U && id.size() < 32; shift += 4U)
      id.push_back(kHex[(word >> shift) & 0xFU]);
  }
  return id;
}

int64_t epochSeconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
