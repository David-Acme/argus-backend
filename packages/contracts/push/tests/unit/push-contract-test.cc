#include <doctest/doctest.h>

#include <push/push-envelope.hxx>
#include <push/push-kind.hxx>
#include <push/push-outcome.hxx>
#include <push/push-priority.hxx>
#include <push/push-provider.hxx>

#include <string>

namespace
{
PushSealedEnvelope validEnvelope()
{
  return {.deliveryId = std::string(16, '\x01'),
          .provider = PushProvider::Fcm,
          .kind = PushKind::Call,
          .priority = PushPriority::High,
          .ttlSeconds = 60,
          .collapseId = "call-0001",
          .token = "fcm-token",
          .enc = std::string(32, '\x02'),
          .ciphertext = std::string(64, '\x03')};
}
}

TEST_CASE("the provider, kind, priority and outcome spell as the wire does")
{
  CHECK(pushProviderToString(PushProvider::ApnsVoip) == "apns_voip");
  CHECK(pushProviderFromString("apns") == PushProvider::Apns);
  CHECK_FALSE(pushProviderFromString("gcm").has_value());
  CHECK(pushKindToString(PushKind::Call) == "call");
  CHECK(pushKindFromString("alert") == PushKind::Alert);
  CHECK(pushPriorityToString(PushPriority::High) == "high");
  CHECK(pushPriorityFromString("normal") == PushPriority::Normal);
  CHECK(pushOutcomeToString(PushOutcome::InvalidToken) == "invalid_token");
  CHECK(pushOutcomeFromString("token_bound") == PushOutcome::TokenBound);
  CHECK_FALSE(pushOutcomeFromString("ok").has_value());
}

TEST_CASE("an envelope that names apns with a call, or voip with an alert, is not well formed")
{
  PushSealedEnvelope envelope = validEnvelope();
  envelope.provider = PushProvider::Apns;
  CHECK_FALSE(push_envelope::wellFormed(envelope));
  envelope.kind = PushKind::Alert;
  CHECK(push_envelope::wellFormed(envelope));
  envelope.provider = PushProvider::ApnsVoip;
  CHECK_FALSE(push_envelope::wellFormed(envelope));
}

TEST_CASE("the ciphertext cap is 2048 bytes and the token cannot be empty")
{
  PushSealedEnvelope envelope = validEnvelope();
  envelope.ciphertext.assign(push_envelope::kMaxCiphertextSize, '\x04');
  CHECK(push_envelope::wellFormed(envelope));
  envelope.ciphertext.push_back('\x04');
  CHECK_FALSE(push_envelope::wellFormed(envelope));
  envelope = validEnvelope();
  envelope.token.clear();
  CHECK_FALSE(push_envelope::wellFormed(envelope));
}

TEST_CASE("a sealed payload parses back to the envelope it was built from")
{
  const PushSealedEnvelope envelope = validEnvelope();
  const auto payload = push_envelope::sealedPayload(envelope);
  if (!payload) {
    FAIL("the sealed payload is empty");
    return;
  }
  PushSealedEnvelope parsed;
  REQUIRE(push_envelope::parseSealedPayload(*payload, parsed));
  CHECK(parsed.deliveryId == envelope.deliveryId);
  CHECK(parsed.collapseId == envelope.collapseId);
  CHECK(parsed.token == envelope.token);
  CHECK(parsed.ciphertext == envelope.ciphertext);
  CHECK(parsed.ttlSeconds == 60);
  CHECK(push_envelope::envelopePrefix(envelope) ==
        payload->substr(0, push_envelope::envelopePrefix(envelope).size()));
}

TEST_CASE("a sealed payload with a trailing byte is refused")
{
  const auto payload = push_envelope::sealedPayload(validEnvelope());
  if (!payload) {
    FAIL("the sealed payload is empty");
    return;
  }
  PushSealedEnvelope parsed;
  CHECK_FALSE(push_envelope::parseSealedPayload(*payload + '\x00', parsed));
}

TEST_CASE("a push result is exactly seventeen bytes with a known outcome")
{
  const PushResultEnvelope result{.deliveryId = std::string(16, '\xAA'),
                                  .outcome = PushOutcome::Unregistered};
  const auto payload = push_envelope::resultPayload(result);
  if (!payload) {
    FAIL("the sealed payload is empty");
    return;
  }
  CHECK(payload->size() == push_envelope::kResultPayloadSize);
  PushResultEnvelope parsed;
  REQUIRE(push_envelope::parseResultPayload(*payload, parsed));
  CHECK(parsed.outcome == PushOutcome::Unregistered);
  CHECK_FALSE(push_envelope::parseResultPayload(*payload + '\x00', parsed));
  std::string unknown = *payload;
  unknown.back() = '\x06';
  CHECK_FALSE(push_envelope::parseResultPayload(unknown, parsed));
}

TEST_CASE("the HPKE info is the label, the key id and the envelope prefix, with no associated data")
{
  const PushSealedEnvelope envelope = validEnvelope();
  const std::string info = push_envelope::hpkeInfo("test-key-1", envelope);
  const std::string expected = std::string("argus.push.v1") + '\0' + "test-key-1" + '\0' + push_envelope::envelopePrefix(envelope);
  CHECK(info == expected);
  PushSealedEnvelope changed = envelope;
  changed.token = "fcm-token-2";
  CHECK(push_envelope::hpkeInfo("test-key-1", changed) != info);
  CHECK(push_envelope::hpkeInfo("test-key-2", envelope) != info);
}
