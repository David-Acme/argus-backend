#include <doctest/doctest.h>

#include <feature/rtc/dtos/rtc-decline-dto.hxx>
#include <feature/rtc/dtos/rtc-token-dto.hxx>
#include <feature/rtc/infra/notification-call-decliner.hxx>
#include <feature/rtc/infra/notification-call-claimer.hxx>
#include <feature/rtc/services/livekit-token.hxx>
#include <feature/rtc/services/rtc-naming.hxx>
#include <feature/rtc/services/rtc-session-revoker.hxx>
#include <feature/rtc/services/rtc-token-service.hxx>
#include <feature/rtc/services/turn-credentials.hxx>

#include <drogon/utils/coroutine.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <notification/notification-client.hxx>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{

constexpr const char* kKey = "argustestkey";
constexpr const char* kSecret = "0123456789abcdef0123456789abcdef0123456789abcdef";

SyncRtcConfig enabledConfig()
{
  return {.enabled = true,
          .apiKey = kKey,
          .apiSecret = kSecret,
          .serverUrl = "http://127.0.0.1:7880",
          .publicUrl = {},
          .publicPort = 7046,
          .tokenTtl = std::chrono::seconds(600),
          .maxConcurrentCalls = 3,
          .turn = {}};
}

constexpr const char* kTurnSecret = "0123456789abcdef0123456789abcdef";

SyncTurnConfig turnRelay()
{
  return {.host = "turn.argus.example",
          .port = 3478,
          .tlsPort = 5349,
          .secret = kTurnSecret,
          .ttl = std::chrono::seconds(3600)};
}

class FakeVoice final : public RtcVoiceJoiner
{
public:
  explicit FakeVoice(bool accept) : accept_(accept) {}

  drogon::Task<bool> join(argus::voice::v1::RtcJoin join) const override
  {
    {
      std::scoped_lock lock(mutex_);
      joins_.push_back(std::move(join));
    }
    if (onJoin)
      onJoin();
    co_return accept_;
  }

  std::function<void()> onJoin;

  drogon::Task<bool> farewell(RtcFarewellInput input) const override
  {
    std::scoped_lock lock(mutex_);
    farewells_.push_back(input.request.room() + "/" + input.request.user_identity() + "/" +
                         input.request.reason());
    deadlines_.push_back(input.deadline);
    co_return accept_;
  }

  std::vector<std::string> farewells() const
  {
    std::scoped_lock lock(mutex_);
    return farewells_;
  }

  std::vector<std::chrono::milliseconds> deadlines() const
  {
    std::scoped_lock lock(mutex_);
    return deadlines_;
  }

  std::vector<argus::voice::v1::RtcJoin> joins() const
  {
    std::scoped_lock lock(mutex_);
    return joins_;
  }

private:
  bool accept_;
  mutable std::mutex mutex_;
  mutable std::vector<argus::voice::v1::RtcJoin> joins_;
  mutable std::vector<std::string> farewells_;
  mutable std::vector<std::chrono::milliseconds> deadlines_;
};

class FakeCalls final : public RtcCallClaimer
{
public:
  explicit FakeCalls(RtcClaim answer) : answer_(std::move(answer)) {}

  drogon::Task<RtcClaim> claim(RtcClaimInput input) const override
  {
    last = std::move(input);
    co_return answer_;
  }

  mutable RtcClaimInput last;

private:
  RtcClaim answer_;
};

class FakeRooms final : public LiveKitRoomClient
{
public:
  FakeRooms() : LiveKitRoomClient({.serverUrl = "http://127.0.0.1:1", .apiKey = kKey, .apiSecret = kSecret}) {}

  drogon::Task<std::optional<std::vector<std::string>>> listRooms() const override
  {
    ++listCalls;
    if (listCalls <= failLists)
      co_return std::nullopt;
    co_return names;
  }

  std::vector<std::string> names{"u7.rtc-aa", "u7.call-3", "u70.rtc-bb", "u8.rtc-cc"};

  drogon::Task<bool> createRoom(std::string room) const override
  {
    calls.push_back("create " + room);
    co_return createOk;
  }

  drogon::Task<bool> removeParticipant(LiveKitParticipantRef participant) const override
  {
    calls.push_back("remove " + participant.room + "/" + participant.identity);
    removed.push_back(participant.room + "/" + participant.identity);
    co_return participant.room == "u7.rtc-aa";
  }

  drogon::Task<bool> deleteRoom(std::string room) const override
  {
    calls.push_back("delete " + room);
    deleted.push_back(room);
    co_return true;
  }

  drogon::Task<std::optional<std::vector<std::string>>> listParticipants(std::string room) const override
  {
    if (room == "u7.call-3")
      co_return std::vector<std::string>{"argus-voice", "user:7:s1", "user:7:s2"};
    co_return std::vector<std::string>{"argus-voice", "user:7:s1"};
  }

  drogon::Task<bool> silenceParticipant(LiveKitParticipantRef participant) const override
  {
    calls.push_back("silence " + participant.room + "/" + participant.identity);
    co_return silenceEverywhere || participant.room == "u7.rtc-aa";
  }

  bool createOk{true};
  bool silenceEverywhere{false};
  int failLists{0};
  mutable int listCalls{0};
  mutable std::vector<std::string> removed;
  mutable std::vector<std::string> deleted;
  mutable std::vector<std::string> calls;
};

JwtContext caller()
{
  return {.sub = 7,
          .name = "Ana",
          .role = UserRole::Resident,
          .isActive = true,
          .deviceHash = "dev-hash",
          .sessionId = "0123456789abcdef0123456789abcdef"};
}

RtcTokenDto bodyOf(const std::string& raw)
{
  Json::Value json;
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  REQUIRE(reader->parse(raw.data(), raw.data() + raw.size(), &json, nullptr));
  return RtcTokenDto::fromJson(json);
}

auto decoded(const std::string& token)
{
  const auto decodedToken = jwt::decode(token);
  jwt::verify().allow_algorithm(jwt::algorithm::hs256{kSecret}).with_issuer(kKey).verify(decodedToken);
  return decodedToken;
}

int statusOf(const auto& action)
{
  try {
    action();
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
  return 200;
}

}

TEST_CASE("call ids are either a user call or a proactive call, nothing else")
{
  const std::string minted = rtc_naming::mintUserCallId();
  CHECK(minted.size() == 36);
  CHECK(rtc_naming::callKindOf(minted) == rtc_naming::CallKind::User);
  CHECK(minted != rtc_naming::mintUserCallId());
  CHECK(rtc_naming::callKindOf("call-123") == rtc_naming::CallKind::Proactive);
  CHECK(rtc_naming::callKindOf("call-0") == rtc_naming::CallKind::Invalid);
  CHECK(rtc_naming::callKindOf("call-") == rtc_naming::CallKind::Invalid);
  CHECK(rtc_naming::callKindOf("call-12a") == rtc_naming::CallKind::Invalid);
  CHECK(rtc_naming::callKindOf("rtc-0123") == rtc_naming::CallKind::Invalid);
  CHECK(rtc_naming::callKindOf("rtc-0123456789ABCDEF0123456789abcdef") == rtc_naming::CallKind::Invalid);
  CHECK(rtc_naming::callKindOf("u8.rtc-aa") == rtc_naming::CallKind::Invalid);
  CHECK(rtc_naming::roomOf(7, "call-3") == "u7.call-3");
  CHECK(rtc_naming::userIdentityOf(7, "abc") == "user:7:abc");
}

TEST_CASE("the LiveKit url names the host the request reached, unless one is configured")
{
  CHECK(rtc_naming::publicUrlOf({.configured = {}, .host = "argus.local:7025", .port = 7046}) ==
        "wss://argus.local:7046");
  CHECK(rtc_naming::publicUrlOf({.configured = {}, .host = "192.168.1.20", .port = 7046}) ==
        "wss://192.168.1.20:7046");
  CHECK(rtc_naming::publicUrlOf({.configured = {}, .host = "[fe80::1]:7025", .port = 7046}) ==
        "wss://[fe80::1]:7046");
  CHECK(rtc_naming::publicUrlOf({.configured = {}, .host = "evil.com/x?y", .port = 7046}) ==
        "wss://argus.local:7046");
  CHECK(rtc_naming::publicUrlOf({.configured = {}, .host = "", .port = 7046}) == "wss://argus.local:7046");
  CHECK(rtc_naming::publicUrlOf({.configured = "wss://calls.example:443", .host = "x", .port = 7046}) ==
        "wss://calls.example:443");
}

TEST_CASE("a LiveKit token carries exactly the grants it was minted with")
{
  const auto now = std::chrono::system_clock::now();
  const std::string token = mintLiveKitToken({.apiKey = kKey,
                                              .apiSecret = kSecret,
                                              .identity = "user:7:abc",
                                              .kind = {},
                                              .grant = {.room = "u7.rtc-aa",
                                                        .roomJoin = true,
                                                        .roomAdmin = false,
                                                        .roomList = false,
                                                   .roomCreate = false,
                                                        .canPublish = true,
                                                        .publishSources = {"microphone"},
                                                        .canSubscribe = true,
                                                        .canPublishData = true,
                                                        .canUpdateOwnMetadata = false},
                                              .ttl = std::chrono::seconds(600),
                                              .now = now});
  const auto claims = decoded(token);
  CHECK(claims.get_subject() == "user:7:abc");
  const auto video = claims.get_payload_claim("video").to_json();
  CHECK(video["room"] == "u7.rtc-aa");
  CHECK(video["roomJoin"] == true);
  CHECK(video["canPublishSources"] == nlohmann::json::array({"microphone"}));
  CHECK_FALSE(video.contains("roomAdmin"));
  CHECK_FALSE(video.contains("canUpdateOwnMetadata"));
  CHECK_FALSE(claims.has_payload_claim("kind"));
  const auto lifetime = claims.get_expires_at() - claims.get_issued_at();
  CHECK(std::chrono::duration_cast<std::chrono::seconds>(lifetime).count() == 600);
  CHECK_THROWS(jwt::verify()
                   .allow_algorithm(jwt::algorithm::hs256{"another-secret-another-secret-another"})
                   .verify(jwt::decode(token)));
}

TEST_CASE("the token request validates its call id, resume and mode")
{
  CHECK(bodyOf("{}").callId.empty());
  CHECK_FALSE(bodyOf("{}").resume);
  CHECK(bodyOf(R"({"callId":"call-4","resume":true,"mode":"half"})").resume);
  CHECK_THROWS_AS(bodyOf(R"({"callId":"u8.rtc-aa"})"), ValidationException);
  CHECK_THROWS_AS(bodyOf(R"({"callId":5})"), ValidationException);
  CHECK_THROWS_AS(bodyOf(R"({"resume":"yes"})"), ValidationException);
  CHECK_THROWS_AS(bodyOf(R"({"mode":"DUPLEX"})"), ValidationException);
  CHECK_THROWS_AS(bodyOf(R"({"mode":3})"), ValidationException);
}

TEST_CASE("a new call asks argus-voice to join first, then hands the caller its own token")
{
  const auto voice = std::make_shared<FakeVoice>(true);
  const auto rooms = std::make_shared<FakeRooms>();
  const RtcTokenService service({.config = enabledConfig(), .voice = voice, .calls = nullptr, .directory = nullptr, .rooms = rooms, .decliner = nullptr});
  const ResponseRtcTokenDto response = drogon::sync_wait(
      service.issue({.body = bodyOf("{}"), .caller = caller(), .host = "argus.local:7025"}));

  CHECK(response.url == "wss://argus.local:7046");
  CHECK(rtc_naming::callKindOf(response.callId) == rtc_naming::CallKind::User);
  CHECK(response.room == "u7." + response.callId);
  CHECK(response.identity == "user:7:0123456789abcdef0123456789abcdef");
  CHECK(response.agentIdentity == "argus-voice");
  CHECK_FALSE(response.call.has_value());

  const auto user = decoded(response.token);
  CHECK(user.get_subject() == response.identity);
  CHECK(user.get_payload_claim("video").to_json()["room"] == response.room);
  CHECK(std::chrono::system_clock::to_time_t(user.get_expires_at()) == response.expiresAt);

  const auto joins = voice->joins();
  REQUIRE(joins.size() == 1);
  CHECK(joins[0].room() == response.room);
  CHECK(joins[0].user_identity() == response.identity);
  CHECK(joins[0].identity().user_id() == 7);
  CHECK(joins[0].identity().role() == argus::voice::v1::VOICE_ROLE_RESIDENT);
  CHECK(joins[0].identity().device_hash() == "dev-hash");
  CHECK(joins[0].mode() == argus::voice::v1::VOICE_MODE_DUPLEX);
  CHECK(joins[0].opening_line().empty());
  const auto agent = decoded(joins[0].agent_token());
  CHECK(agent.get_subject() == "argus-voice");
  CHECK(agent.get_payload_claim("kind").as_string() == "agent");
  CHECK(agent.get_payload_claim("video").to_json()["canUpdateOwnMetadata"] == true);
  CHECK(rooms->calls == std::vector<std::string>{"create " + response.room});
}

TEST_CASE("a room LiveKit refuses to create is never handed to the agent")
{
  const auto voice = std::make_shared<FakeVoice>(true);
  const auto rooms = std::make_shared<FakeRooms>();
  rooms->createOk = false;
  const RtcTokenService service(
      {.config = enabledConfig(), .voice = voice, .calls = nullptr, .directory = nullptr, .rooms = rooms, .decliner = nullptr});
  CHECK(statusOf([&] {
          (void)drogon::sync_wait(service.issue({.body = bodyOf("{}"), .caller = caller(), .host = "argus.local"}));
        }) == 503);
  CHECK(voice->joins().empty());
  CHECK(SyncRtcConfig{}.tokenTtl == std::chrono::seconds(60));
}

TEST_CASE("a user holds at most the configured number of calls at once")
{
  const auto voice = std::make_shared<FakeVoice>(true);
  const auto rooms = std::make_shared<FakeRooms>();
  const std::string held = rtc_naming::mintUserCallId();
  rooms->names = {"u7." + held, "u7.call-3", "u70.rtc-bb", "u8.rtc-cc"};
  SyncRtcConfig config = enabledConfig();
  config.maxConcurrentCalls = 2;
  const RtcTokenService service(
      {.config = config, .voice = voice, .calls = nullptr, .directory = nullptr, .rooms = rooms, .decliner = nullptr});
  const auto issue = [&service](const std::string& body) {
    return drogon::sync_wait(
        service.issue({.body = bodyOf(body), .caller = caller(), .host = "argus.local"}));
  };

  try {
    static_cast<void>(issue("{}"));
    FAIL("a third call must be refused");
  }
  catch (const ResponseException& error) {
    CHECK(error.statusCode() == 429);
    CHECK(error.errorCode() == "TOO_MANY_REQUESTS");
  }
  CHECK(voice->joins().empty());
  CHECK(rooms->calls.empty());

  const auto resumed = issue(R"({"callId":")" + held + R"(","resume":true})");
  CHECK(resumed.room == "u7." + held);

  rooms->names = {"u7." + held, "u70.rtc-bb", "u8.rtc-cc"};
  int nestedStatus = 0;
  bool nested = false;
  voice->onJoin = [&] {
    if (nested)
      return;
    nested = true;
    nestedStatus = statusOf([&] { static_cast<void>(issue("{}")); });
  };
  const auto second = issue("{}");
  CHECK(second.room.starts_with("u7.rtc-"));
  CHECK(nestedStatus == 429);
  CHECK(statusOf([&] { static_cast<void>(issue("{}")); }) == 200);

  rooms->failLists = rooms->listCalls + 1;
  CHECK(statusOf([&] { static_cast<void>(issue("{}")); }) == 503);
  CHECK(SyncRtcConfig{}.maxConcurrentCalls == 2);
}

TEST_CASE("a resume keeps the room and tells the agent not to greet")
{
  const auto voice = std::make_shared<FakeVoice>(true);
  const RtcTokenService service({.config = enabledConfig(), .voice = voice, .calls = nullptr, .directory = nullptr, .rooms = nullptr, .decliner = nullptr});
  const std::string callId = rtc_naming::mintUserCallId();
  const auto response = drogon::sync_wait(service.issue(
      {.body = bodyOf(R"({"callId":")" + callId + R"(","resume":true,"mode":"half"})"),
       .caller = caller(),
       .host = "argus.local"}));
  CHECK(response.room == "u7." + callId);
  REQUIRE(voice->joins().size() == 1);
  CHECK(voice->joins()[0].resume());
  CHECK(voice->joins()[0].mode() == argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
}

TEST_CASE("a proactive call is claimed first, and the opening line goes to the agent only")
{
  const auto voice = std::make_shared<FakeVoice>(true);
  const auto calls = std::make_shared<FakeCalls>(RtcClaim{.status = RtcClaimStatus::Claimed,
                                                          .openingLine = "Hay alguien en el patio.",
                                                          .laterLines = {},
                                                          .lang = "es",
                                                          .kind = "guard_episode",
                                                          .summary = "Persona desconocida",
                                                          .cameraId = 6,
                                                          .cameraName = "Patio",
                                                          .episodeId = 41});
  const RtcTokenService service({.config = enabledConfig(), .voice = voice, .calls = calls, .directory = nullptr, .rooms = nullptr, .decliner = nullptr});
  const auto response = drogon::sync_wait(
      service.issue({.body = bodyOf(R"({"callId":"call-41"})"), .caller = caller(), .host = "argus.local"}));
  CHECK(calls->last.callId == "call-41");
  CHECK(calls->last.userId == 7);
  CHECK(calls->last.sessionId == caller().sessionId);
  REQUIRE(response.call.has_value());
  const ResponseRtcCallDto call = response.call.value_or(ResponseRtcCallDto{});
  CHECK(call.cameraName == "Patio");
  CHECK(call.episodeId == 41);
  CHECK(response.toJson()["call"]["kind"].asString() == "guard_episode");
  CHECK(response.toJson().toStyledString().find("Hay alguien") == std::string::npos);
  REQUIRE(voice->joins().size() == 1);
  CHECK(voice->joins()[0].opening_line() == "Hay alguien en el patio.");
  CHECK(voice->joins()[0].call_kind() == "guard_episode");
  CHECK(voice->joins()[0].identity().language() == argus::voice::v1::VOICE_LANGUAGE_ES);
}

TEST_CASE("a claim that fails maps to the call outcome, never to a token")
{
  const auto voice = std::make_shared<FakeVoice>(true);
  const auto statusFor = [&](RtcClaimStatus status) {
    const RtcTokenService service({.config = enabledConfig(),
                                   .voice = voice,
                                   .calls = std::make_shared<FakeCalls>(RtcClaim{.status = status,
                                                                                  .openingLine = {},
                                                                                  .laterLines = {},
                                                                                  .lang = {},
                                                                                  .kind = {},
                                                                                  .summary = {},
                                                                                  .cameraId = 0,
                                                                                  .cameraName = {},
                                                                                  .episodeId = 0}),
                                   .directory = nullptr, .rooms = nullptr, .decliner = nullptr});
    return statusOf([&] {
      (void)drogon::sync_wait(
          service.issue({.body = bodyOf(R"({"callId":"call-9"})"), .caller = caller(), .host = "argus.local"}));
    });
  };
  CHECK(statusFor(RtcClaimStatus::Taken) == 409);
  CHECK(statusFor(RtcClaimStatus::Expired) == 410);
  CHECK(statusFor(RtcClaimStatus::NotFound) == 404);
  CHECK(statusFor(RtcClaimStatus::Unavailable) == 503);
  CHECK(voice->joins().empty());
}

TEST_CASE("without LiveKit, without the agent or without a session id the route answers 503")
{
  const auto refused = [](RtcTokenServiceInput input, JwtContext who) {
    const RtcTokenService service(std::move(input));
    return statusOf([&] {
      (void)drogon::sync_wait(service.issue({.body = bodyOf("{}"), .caller = who, .host = "argus.local"}));
    });
  };
  SyncRtcConfig disabled = enabledConfig();
  disabled.enabled = false;
  CHECK(refused({.config = disabled, .voice = std::make_shared<FakeVoice>(true), .calls = nullptr, .directory = nullptr, .rooms = nullptr, .decliner = nullptr},
                caller()) == 503);
  CHECK(refused({.config = enabledConfig(), .voice = std::make_shared<FakeVoice>(false), .calls = nullptr, .directory = nullptr, .rooms = nullptr, .decliner = nullptr},
                caller()) == 503);
  CHECK(refused({.config = enabledConfig(), .voice = nullptr, .calls = nullptr, .directory = nullptr, .rooms = nullptr, .decliner = nullptr}, caller()) == 503);
  JwtContext legacy = caller();
  legacy.sessionId.clear();
  CHECK(refused({.config = enabledConfig(), .voice = std::make_shared<FakeVoice>(true), .calls = nullptr, .directory = nullptr, .rooms = nullptr, .decliner = nullptr},
                legacy) == 503);
  CHECK(refused({.config = enabledConfig(), .voice = std::make_shared<FakeVoice>(true), .calls = nullptr, .directory = nullptr, .rooms = nullptr, .decliner = nullptr},
                caller()) == 200);
}

TEST_CASE("the notification claim reply maps onto the route's claim")
{
  NotificationCallClaimResult result;
  result.outcome = NotificationRpcOutcome::Success;
  result.response.set_status(argus::notification::v1::CALL_CLAIM_STATUS_CLAIMED);
  result.response.set_opening_line("Hola");
  result.response.set_camera_id(6);
  CHECK(NotificationCallClaimer::claimOf(result).status == RtcClaimStatus::Claimed);
  CHECK(NotificationCallClaimer::claimOf(result).openingLine == "Hola");
  result.response.set_status(argus::notification::v1::CALL_CLAIM_STATUS_TAKEN);
  CHECK(NotificationCallClaimer::claimOf(result).status == RtcClaimStatus::Taken);
  CHECK(NotificationCallClaimer::claimOf(result).openingLine.empty());
  result.response.set_status(argus::notification::v1::CALL_CLAIM_STATUS_UNSPECIFIED);
  CHECK(NotificationCallClaimer::claimOf(result).status == RtcClaimStatus::Unavailable);
  result.outcome = NotificationRpcOutcome::Unavailable;
  result.response.set_status(argus::notification::v1::CALL_CLAIM_STATUS_CLAIMED);
  CHECK(NotificationCallClaimer::claimOf(result).status == RtcClaimStatus::Unavailable);
}

TEST_CASE("a revoked session is silenced, hears Argus say goodbye, then leaves; only its own calls")
{
  const auto rooms = std::make_shared<FakeRooms>();
  const auto voice = std::make_shared<FakeVoice>(true);
  const int removed = drogon::sync_wait(RtcSessionRevoker::revoke(
      {.rooms = rooms,
       .voice = voice,
       .end = {.userId = 7, .sessionId = std::string("s1"), .cause = "revokedByOwner"},
       .farewellBudget = RtcSessionRevoker::kFarewellBudget,
       .listRetries = {}}));
  CHECK(removed == 1);
  CHECK(rooms->calls == std::vector<std::string>{"silence u7.rtc-aa/user:7:s1", "delete u7.rtc-aa",
                                                 "silence u7.call-3/user:7:s1"});
  CHECK(voice->farewells() == std::vector<std::string>{"u7.rtc-aa/user:7:s1/revokedByOwner"});
  REQUIRE(voice->deadlines().size() == 1);
  CHECK(voice->deadlines()[0] <= RtcSessionRevoker::kFarewellBudget);
  CHECK(voice->deadlines()[0] > std::chrono::milliseconds(2000));
  CHECK(rooms->deleted == std::vector<std::string>{"u7.rtc-aa"});
  CHECK(rooms->removed.empty());
}

TEST_CASE("a revoked session leaves a room another of its user's devices still holds")
{
  const auto rooms = std::make_shared<FakeRooms>();
  rooms->silenceEverywhere = true;
  CHECK(drogon::sync_wait(RtcSessionRevoker::revoke(
            {.rooms = rooms,
             .voice = nullptr,
             .end = {.userId = 7, .sessionId = std::string("s1"), .cause = "revoked"},
             .farewellBudget = RtcSessionRevoker::kFarewellBudget,
             .listRetries = {}})) == 1);
  CHECK(rooms->calls == std::vector<std::string>{"silence u7.rtc-aa/user:7:s1", "delete u7.rtc-aa",
                                                 "silence u7.call-3/user:7:s1", "remove u7.call-3/user:7:s1"});
}

TEST_CASE("the room list is retried before a revocation gives up")
{
  const auto flaky = std::make_shared<FakeRooms>();
  flaky->failLists = 2;
  CHECK(drogon::sync_wait(RtcSessionRevoker::revoke(
            {.rooms = flaky,
             .voice = nullptr,
             .end = {.userId = 7, .sessionId = std::string("s1"), .cause = "revoked"},
             .farewellBudget = RtcSessionRevoker::kFarewellBudget,
             .listRetries = {std::chrono::milliseconds(0), std::chrono::milliseconds(0)}})) == 1);
  CHECK(flaky->listCalls == 3);

  const auto down = std::make_shared<FakeRooms>();
  down->failLists = 10;
  CHECK(drogon::sync_wait(RtcSessionRevoker::revoke(
            {.rooms = down,
             .voice = nullptr,
             .end = {.userId = 7, .sessionId = std::string("s1"), .cause = "revoked"},
             .farewellBudget = RtcSessionRevoker::kFarewellBudget,
             .listRetries = {std::chrono::milliseconds(0), std::chrono::milliseconds(0)}})) == 0);
  CHECK(down->listCalls == 3);
  CHECK(down->calls.empty());
  CHECK(RtcSessionRevoker::defaultListRetries().size() == 3);
}

TEST_CASE("a disabled account silences every session of the user, says goodbye and ends the rooms")
{
  const auto rooms = std::make_shared<FakeRooms>();
  const auto voice = std::make_shared<FakeVoice>(false);
  CHECK(drogon::sync_wait(RtcSessionRevoker::revoke({.rooms = rooms,
                                                     .voice = voice,
                                                     .end = {.userId = 7, .sessionId = std::nullopt, .cause = "accountDisabled"},
                                                     .farewellBudget = RtcSessionRevoker::kFarewellBudget,
       .listRetries = {}})) == 2);
  CHECK(rooms->calls == std::vector<std::string>{"silence u7.rtc-aa/user:7:s1", "delete u7.rtc-aa",
                                                 "silence u7.call-3/user:7:s1", "silence u7.call-3/user:7:s2",
                                                 "delete u7.call-3"});
  CHECK(voice->farewells() == std::vector<std::string>{"u7.rtc-aa//accountDisabled", "u7.call-3//accountDisabled"});
}

TEST_CASE("the goodbye never stretches the revocation past its budget")
{
  const auto rooms = std::make_shared<FakeRooms>();
  const auto voice = std::make_shared<FakeVoice>(true);
  CHECK(drogon::sync_wait(RtcSessionRevoker::revoke({.rooms = rooms,
                                                     .voice = voice,
                                                     .end = {.userId = 7, .sessionId = std::string("s1"), .cause = ""},
                                                     .farewellBudget = std::chrono::milliseconds(0),
       .listRetries = {}})) == 1);
  REQUIRE(voice->deadlines().size() == 1);
  CHECK(voice->deadlines()[0] == std::chrono::milliseconds(0));
  const auto none = std::make_shared<FakeRooms>();
  CHECK(drogon::sync_wait(RtcSessionRevoker::revoke({.rooms = none,
                                                     .voice = nullptr,
                                                     .end = {.userId = 7, .sessionId = std::string("s1"), .cause = ""},
                                                     .farewellBudget = RtcSessionRevoker::kFarewellBudget,
       .listRetries = {}})) == 1);
}

namespace
{
class FakeDecliner final : public RtcCallDecliner
{
public:
  explicit FakeDecliner(RtcDeclineStatus answer) : answer_(answer) {}

  drogon::Task<RtcDecline> decline(RtcDeclineInput input) const override
  {
    last = std::move(input);
    co_return RtcDecline{.status = answer_};
  }

  mutable RtcDeclineInput last;

private:
  RtcDeclineStatus answer_;
};

RtcDeclineDto declineBodyOf(const std::string& raw)
{
  Json::Value json;
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  REQUIRE(reader->parse(raw.data(), raw.data() + raw.size(), &json, nullptr));
  return RtcDeclineDto::fromJson(json);
}
}

TEST_CASE("a decline answers declined, and a second decline of the same call is answered the same way")
{
  const auto decliner = std::make_shared<FakeDecliner>(RtcDeclineStatus::Declined);
  const RtcTokenService service({.config = enabledConfig(), .voice = nullptr, .calls = nullptr,
                                 .directory = nullptr, .rooms = nullptr, .decliner = decliner});
  const auto first = drogon::sync_wait(
      service.decline({.body = declineBodyOf(R"({"callId":"call-9"})"), .caller = caller()}));
  CHECK(first.declined);
  CHECK(statusOf([&] {
    (void)drogon::sync_wait(
        service.decline({.body = declineBodyOf(R"({"callId":"call-9"})"), .caller = caller()}));
  }) == 200);
}

TEST_CASE("a decline names the caller as the callee and refuses with the claim codes")
{
  const auto declineStatus = [](RtcDeclineStatus answer) {
    const auto decliner = std::make_shared<FakeDecliner>(answer);
    const RtcTokenService service({.config = enabledConfig(), .voice = nullptr, .calls = nullptr,
                                   .directory = nullptr, .rooms = nullptr, .decliner = decliner});
    const int status = statusOf([&] {
      (void)drogon::sync_wait(
          service.decline({.body = declineBodyOf(R"({"callId":"call-9"})"), .caller = caller()}));
    });
    CHECK(decliner->last.userId == 7);
    CHECK(decliner->last.callId == "call-9");
    return status;
  };
  CHECK(declineStatus(RtcDeclineStatus::Taken) == 409);
  CHECK(declineStatus(RtcDeclineStatus::Expired) == 410);
  CHECK(declineStatus(RtcDeclineStatus::NotFound) == 404);
  CHECK(declineStatus(RtcDeclineStatus::Unavailable) == 503);
}

TEST_CASE("the notification's answer maps onto the decline codes, and only silence is unavailable")
{
  const auto mapped = [](NotificationRpcOutcome outcome, argus::notification::v1::CallEndStatus status) {
    return NotificationCallDecliner::declineOf({.outcome = outcome, .status = status}).status;
  };
  using Status = argus::notification::v1::CallEndStatus;
  CHECK(mapped(NotificationRpcOutcome::Success, Status::CALL_END_STATUS_ENDED) == RtcDeclineStatus::Declined);
  CHECK(mapped(NotificationRpcOutcome::Rejected, Status::CALL_END_STATUS_EXPIRED) == RtcDeclineStatus::Expired);
  CHECK(mapped(NotificationRpcOutcome::Rejected, Status::CALL_END_STATUS_TAKEN) == RtcDeclineStatus::Taken);
  CHECK(mapped(NotificationRpcOutcome::Rejected, Status::CALL_END_STATUS_NOT_FOUND) == RtcDeclineStatus::NotFound);
  CHECK(mapped(NotificationRpcOutcome::Rejected, Status::CALL_END_STATUS_ENDED) == RtcDeclineStatus::Unavailable);
  CHECK(mapped(NotificationRpcOutcome::Unavailable, Status::CALL_END_STATUS_EXPIRED) == RtcDeclineStatus::Unavailable);
  CHECK(mapped(NotificationRpcOutcome::Conflict, Status::CALL_END_STATUS_TAKEN) == RtcDeclineStatus::Unavailable);
}

TEST_CASE("a TURN credential is the RFC REST pair: expiry and subject, HMAC-SHA1 in base64")
{
  const TurnCredential credential =
      turn_credentials::make({.secret = kTurnSecret, .subject = "42", .expiresAt = 1800000000});
  CHECK(credential.username == "1800000000:42");
  CHECK(credential.credential == "nvgSm1oHwSfK7YbCFCNNQAthpc0=");
  CHECK(credential.expiresAt == 1800000000);
}

TEST_CASE("the TURN urls name udp, tcp and, when a TLS port is set, turns")
{
  SyncTurnConfig relay = turnRelay();
  CHECK(turn_credentials::urls(relay) ==
        std::vector<std::string>{"turn:turn.argus.example:3478?transport=udp",
                                 "turn:turn.argus.example:3478?transport=tcp",
                                 "turns:turn.argus.example:5349?transport=tcp"});
  relay.tlsPort = 0;
  CHECK(turn_credentials::urls(relay).size() == 2);
}

TEST_CASE("no relay is offered without a host, with a short or placeholder secret, or with a bad host")
{
  CHECK(turn_credentials::enabled(turnRelay()));
  SyncTurnConfig relay = turnRelay();
  relay.host.clear();
  CHECK_FALSE(turn_credentials::enabled(relay));
  CHECK(turn_credentials::iceServers({.relay = relay, .subject = "7", .now = 1000}).servers.empty());
  relay = turnRelay();
  relay.secret = "short";
  CHECK_FALSE(turn_credentials::enabled(relay));
  relay = turnRelay();
  relay.secret = "CHANGE_ME_TURN_SECRET_0123456789abcdef";
  CHECK_FALSE(turn_credentials::enabled(relay));
  relay = turnRelay();
  relay.host = "turn.example\nrealm=x";
  CHECK_FALSE(turn_credentials::enabled(relay));
}

TEST_CASE("the ice route hands the caller one relay whose credential expires after the ttl")
{
  SyncRtcConfig config = enabledConfig();
  config.turn = turnRelay();
  const RtcTokenService service({.config = config,
                                 .voice = nullptr,
                                 .calls = nullptr,
                                 .directory = nullptr,
                                 .rooms = nullptr,
                                 .decliner = nullptr});
  const ResponseRtcIceDto ice = service.ice({.caller = caller(), .now = 1000});
  REQUIRE(ice.iceServers.size() == 1);
  CHECK(ice.expiresAt == 4600);
  CHECK(ice.iceServers[0]["username"].asString() == "4600:7");
  CHECK(ice.iceServers[0]["credential"].asString() ==
        turn_credentials::make({.secret = kTurnSecret, .subject = "7", .expiresAt = 4600}).credential);
  CHECK(ice.iceServers[0]["urls"].size() == 3);
  const Json::Value json = ice.toJson();
  CHECK(json["iceServers"].isArray());
  CHECK(json["expiresAt"].asInt64() == 4600);
}

TEST_CASE("without a relay the ice route and the call token both carry an empty list")
{
  const auto voice = std::make_shared<FakeVoice>(true);
  const auto rooms = std::make_shared<FakeRooms>();
  const RtcTokenService service({.config = enabledConfig(),
                                 .voice = voice,
                                 .calls = nullptr,
                                 .directory = nullptr,
                                 .rooms = rooms,
                                 .decliner = nullptr,
                                 .watch = nullptr});
  const ResponseRtcIceDto ice = service.ice({.caller = caller(), .now = 1000});
  CHECK(ice.iceServers.empty());
  CHECK(ice.expiresAt == 0);
  const ResponseRtcTokenDto response = drogon::sync_wait(
      service.issue({.body = bodyOf("{}"), .caller = caller(), .host = "argus.local:7025"}));
  CHECK(response.iceServers.isArray());
  CHECK(response.iceServers.empty());
  CHECK(response.toJson()["iceServers"].isArray());
}

namespace
{

class FakeWatch final : public RtcWatchDirectory
{
public:
  drogon::Task<std::optional<std::vector<argus::voice::v1::WatchConversation>>> list(int64_t userId) const override
  {
    listedFor = userId;
    co_return conversations;
  }

  drogon::Task<std::optional<bool>> end(RtcWatchEnd input) const override
  {
    ended.push_back(input.conversationId);
    co_return input.conversationId == conversations.front().conversation_id();
  }

  drogon::Task<std::optional<argus::voice::v1::ReplyWatchVisitorResponse>> reply(RtcWatchReply input) const override
  {
    replies.push_back(input);
    if (!reachable)
      co_return std::nullopt;
    argus::voice::v1::ReplyWatchVisitorResponse answer;
    answer.set_outcome(outcome);
    answer.set_reply_until(4200);
    co_return answer;
  }

  std::vector<argus::voice::v1::WatchConversation> conversations;
  mutable int64_t listedFor{0};
  mutable std::vector<std::string> ended;
  mutable std::vector<RtcWatchReply> replies;
  argus::voice::v1::WatchReplyOutcome outcome{argus::voice::v1::WATCH_REPLY_OUTCOME_QUEUED};
  bool reachable{true};
};

struct WatchRow
{
  std::string id;
  std::string state;
  int64_t until{0};
};

argus::voice::v1::WatchConversation watchOf(const WatchRow& row)
{
  const auto& [id, state, until] = row;
  argus::voice::v1::WatchConversation conversation;
  conversation.set_conversation_id(id);
  conversation.set_user_id(7);
  conversation.set_camera_id(3);
  conversation.set_camera_name("Sala");
  conversation.set_state(state);
  conversation.set_handoff("camera");
  conversation.set_audience("member");
  conversation.set_resumable_until(until);
  auto* turn = conversation.add_turns();
  turn->set_id(1);
  turn->set_role("user");
  turn->set_text("Argus, ¿qué tengo hoy?");
  turn->set_at(100);
  turn->set_final(true);
  return conversation;
}

constexpr const char* kLiveId = "0123456789abcdef0123456789abcdef";
constexpr const char* kGoneId = "fedcba9876543210fedcba9876543210";

}

TEST_CASE("a camera conversation is continued on the phone only while it is live or resumable")
{
  CHECK(isWatchConversationId(kLiveId));
  CHECK_FALSE(isWatchConversationId("0123"));
  CHECK_FALSE(isWatchConversationId("0123456789ABCDEF0123456789ABCDEF"));
  CHECK(bodyOf(std::string(R"({"watchConversationId":")") + kLiveId + "\"}").watchConversationId == kLiveId);
  CHECK_THROWS_AS(bodyOf(R"({"watchConversationId":"nope"})"), ValidationException);
  CHECK_THROWS_AS(bodyOf(std::string(R"({"callId":"call-4","watchConversationId":")") + kLiveId + "\"}"),
                  ValidationException);

  const auto voice = std::make_shared<FakeVoice>(true);
  const auto rooms = std::make_shared<FakeRooms>();
  const auto watch = std::make_shared<FakeWatch>();
  const int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  watch->conversations = {watchOf({.id = kLiveId, .state = "listening", .until = 0}), watchOf({.id = kGoneId, .state = "resumable", .until = now - 5})};
  const RtcTokenService service({.config = enabledConfig(),
                                 .voice = voice,
                                 .calls = nullptr,
                                 .directory = nullptr,
                                 .rooms = rooms,
                                 .decliner = nullptr,
                                 .watch = watch});
  const ResponseRtcTokenDto response = drogon::sync_wait(service.issue(
      {.body = bodyOf(std::string(R"({"watchConversationId":")") + kLiveId + "\"}"), .caller = caller(), .host = "argus.local"}));
  CHECK(response.callId == std::string("rtc-") + kLiveId);
  const auto joins = voice->joins();
  REQUIRE(joins.size() == 1);
  CHECK(joins[0].watch_conversation_id() == kLiveId);
  CHECK(joins[0].call_kind() == "watch");

  CHECK(statusOf([&] {
          static_cast<void>(drogon::sync_wait(
              service.issue({.body = bodyOf(std::string(R"({"watchConversationId":")") + kGoneId + "\"}"),
                             .caller = caller(),
                             .host = "argus.local"})));
        }) == 410);
  CHECK(statusOf([&] {
          static_cast<void>(drogon::sync_wait(
              service.issue({.body = bodyOf(R"({"watchConversationId":"00000000000000000000000000000000"})"),
                             .caller = caller(),
                             .host = "argus.local"})));
        }) == 404);

  const ResponseRtcWatchDto listed = drogon::sync_wait(service.watchConversations(caller()));
  CHECK(watch->listedFor == 7);
  const Json::Value json = listed.toJson();
  REQUIRE(json["conversations"].size() == 2);
  const Json::Value& first = json["conversations"][0];
  CHECK(first["conversationId"] == kLiveId);
  CHECK(first["cameraName"] == "Sala");
  CHECK(first["state"] == "listening");
  CHECK(first["handoff"] == "camera");
  CHECK(first["audience"] == "member");
  CHECK(first["resumableUntil"].isNull());
  CHECK(first["turns"][0]["role"] == "user");
  CHECK(first["turns"][0]["final"].asBool());

  CHECK(drogon::sync_wait(service.endWatch({.body = RtcWatchEndDto::fromPath(kLiveId), .caller = caller()})).ended);
  CHECK(statusOf([&] {
          static_cast<void>(
              drogon::sync_wait(service.endWatch({.body = RtcWatchEndDto::fromPath(kGoneId), .caller = caller()})));
        }) == 404);
  CHECK_THROWS_AS(RtcWatchEndDto::fromPath("x"), ValidationException);
  CHECK(watchExpired(watchOf({.id = kGoneId, .state = "ended", .until = 0}), now));
  CHECK_FALSE(watchExpired(watchOf({.id = kGoneId, .state = "resumable", .until = now + 60}), now));
}

TEST_CASE("a member answers a visitor's message for the camera, and only the voice service decides whether it may")
{
  const auto watch = std::make_shared<FakeWatch>();
  const RtcTokenService service({.config = enabledConfig(),
                                 .voice = std::make_shared<FakeVoice>(true),
                                 .calls = nullptr,
                                 .directory = nullptr,
                                 .rooms = std::make_shared<FakeRooms>(),
                                 .decliner = nullptr,
                                 .watch = watch});
  Json::Value body(Json::objectValue);
  body["text"] = "Déjelo en la puerta, gracias.";
  const RtcWatchReplyDto dto = RtcWatchReplyDto::fromJson(body, kLiveId);
  const ResponseRtcWatchReplyDto queued = drogon::sync_wait(service.replyWatch({.body = dto, .caller = caller()}));
  CHECK(queued.toJson()["state"] == "queued");
  CHECK(queued.toJson()["replyUntil"].asInt64() == 4200);
  REQUIRE(watch->replies.size() == 1);
  CHECK(watch->replies[0].userId == 7);
  CHECK(watch->replies[0].conversationId == kLiveId);
  CHECK(watch->replies[0].text == "Déjelo en la puerta, gracias.");

  watch->outcome = argus::voice::v1::WATCH_REPLY_OUTCOME_SAID;
  CHECK(drogon::sync_wait(service.replyWatch({.body = dto, .caller = caller()})).toJson()["state"] == "said");
  const auto statusFor = [&](argus::voice::v1::WatchReplyOutcome outcome) {
    watch->outcome = outcome;
    return statusOf([&] { static_cast<void>(drogon::sync_wait(service.replyWatch({.body = dto, .caller = caller()}))); });
  };
  CHECK(statusFor(argus::voice::v1::WATCH_REPLY_OUTCOME_NOT_FOUND) == 404);
  CHECK(statusFor(argus::voice::v1::WATCH_REPLY_OUTCOME_EXPIRED) == 410);
  CHECK(statusFor(argus::voice::v1::WATCH_REPLY_OUTCOME_TOO_LONG) == 422);
  watch->reachable = false;
  CHECK(statusOf([&] { static_cast<void>(drogon::sync_wait(service.replyWatch({.body = dto, .caller = caller()}))); }) == 503);

  Json::Value empty(Json::objectValue);
  CHECK_THROWS_AS(RtcWatchReplyDto::fromJson(empty, kLiveId), ValidationException);
  CHECK_THROWS_AS(RtcWatchReplyDto::fromJson(body, "nope"), ValidationException);
}
