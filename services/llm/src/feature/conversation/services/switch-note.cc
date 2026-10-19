#include <feature/conversation/services/switch-note.hxx>

namespace
{
std::string spanishLeg(ConversationLegKind leg, std::string_view place)
{
  switch (leg) {
    case ConversationLegKind::Camera:
      if (place.empty())
        return "la cámara";
      return "la cámara de " + std::string(place);
    case ConversationLegKind::Phone:
      return "el teléfono";
    case ConversationLegKind::Laptop:
      return "la computadora";
  }
  return {};
}

std::string englishLeg(ConversationLegKind leg, std::string_view place)
{
  switch (leg) {
    case ConversationLegKind::Camera:
      if (place.empty())
        return "the camera";
      return "the camera in " + std::string(place);
    case ConversationLegKind::Phone:
      return "the phone";
    case ConversationLegKind::Laptop:
      return "the laptop";
  }
  return {};
}
}

std::string switchNote(const SwitchNoteInput& input)
{
  if (input.lang == "en")
    return "The conversation continues from " + englishLeg(input.to, input.cameraPlaceWithArticle) +
           "; it was on " + englishLeg(input.from, input.cameraPlaceWithArticle) + ".";
  return "La conversación continúa desde " + spanishLeg(input.to, input.cameraPlaceWithArticle) +
         "; antes estaban en " + spanishLeg(input.from, input.cameraPlaceWithArticle) + ".";
}
