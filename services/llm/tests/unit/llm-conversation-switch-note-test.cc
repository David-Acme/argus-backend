#include <doctest/doctest.h>

#include <feature/conversation/services/switch-note.hxx>

TEST_CASE("a Spanish switch from the camera to the phone names both legs")
{
  const SwitchNoteInput input{.lang = "es",
                              .from = ConversationLegKind::Camera,
                              .to = ConversationLegKind::Phone,
                              .cameraPlaceWithArticle = "la sala"};
  CHECK(switchNote(input) ==
        "La conversación continúa desde el teléfono; antes estaban en la cámara de la sala.");
}

TEST_CASE("a Spanish switch from the phone to the laptop uses the laptop name")
{
  const SwitchNoteInput input{.lang = "es",
                              .from = ConversationLegKind::Phone,
                              .to = ConversationLegKind::Laptop,
                              .cameraPlaceWithArticle = "la sala"};
  CHECK(switchNote(input) == "La conversación continúa desde la computadora; antes estaban en el teléfono.");
}

TEST_CASE("a switch from a camera with no place name says 'la cámara' without a dangling preposition")
{
  const SwitchNoteInput input{.lang = "es",
                              .from = ConversationLegKind::Camera,
                              .to = ConversationLegKind::Phone,
                              .cameraPlaceWithArticle = ""};
  CHECK(switchNote(input) == "La conversación continúa desde el teléfono; antes estaban en la cámara.");
}

TEST_CASE("an English switch names the camera place in English")
{
  const SwitchNoteInput input{.lang = "en",
                              .from = ConversationLegKind::Camera,
                              .to = ConversationLegKind::Phone,
                              .cameraPlaceWithArticle = "the living room"};
  CHECK(switchNote(input) ==
        "The conversation continues from the phone; it was on the camera in the living room.");
}
