#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/voice/offer-reply.hxx>

TEST_CASE("A short yes to a camera offer accepts it, in either language")
{
  CHECK(offerReplyOf("Sí, muéstramela.", VoiceLang::Es) == OfferReply::Accept);
  CHECK(offerReplyOf("¡Vale!", VoiceLang::Es) == OfferReply::Accept);
  CHECK(offerReplyOf("enséñamela por favor", VoiceLang::Es) == OfferReply::Accept);
  CHECK(offerReplyOf("SÍ", VoiceLang::Es) == OfferReply::Accept);
  CHECK(offerReplyOf("Yes, show me.", VoiceLang::En) == OfferReply::Accept);
}

TEST_CASE("A short no declines it")
{
  CHECK(offerReplyOf("No, ahora no.", VoiceLang::Es) == OfferReply::Decline);
  CHECK(offerReplyOf("Déjalo.", VoiceLang::Es) == OfferReply::Decline);
  CHECK(offerReplyOf("Not now.", VoiceLang::En) == OfferReply::Decline);
}

TEST_CASE("Anything longer or mixed goes to the model")
{
  CHECK(offerReplyOf("Sí, pero antes dime qué tengo hoy en la agenda", VoiceLang::Es) == OfferReply::Other);
  CHECK(offerReplyOf("Sí, no tardes", VoiceLang::Es) == OfferReply::Other);
  CHECK(offerReplyOf("¿Qué hora es?", VoiceLang::Es) == OfferReply::Other);
  CHECK(offerReplyOf("", VoiceLang::Es) == OfferReply::Other);
  CHECK(offerReplyOf("nosotros", VoiceLang::Es) == OfferReply::Other);
}
