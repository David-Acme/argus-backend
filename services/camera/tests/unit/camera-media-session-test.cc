#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/media/media-session-registry.hxx>

#include <drogon/WebSocketConnection.h>
#include <json/value.h>
#include <sync/sync-change.hxx>
#include <text/json-util.hxx>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace
{
class ClosingConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char*, uint64_t, const drogon::WebSocketMessageType) override {}
  void send(std::string_view, const drogon::WebSocketMessageType) override {}
  void sendJson(const Json::Value&, const drogon::WebSocketMessageType) override {}
  [[nodiscard]] const trantor::InetAddress& localAddr() const override { return addr_; }
  [[nodiscard]] const trantor::InetAddress& peerAddr() const override { return addr_; }
  [[nodiscard]] bool connected() const override { return !closed; }
  [[nodiscard]] bool disconnected() const override { return closed; }
  void shutdown(const drogon::CloseCode code, const std::string& reason) override
  {
    closed = true;
    closeCode = code;
    closeReason = reason;
  }
  void forceClose() override { closed = true; }
  void setPingMessage(const std::string&, const std::chrono::duration<double>&) override {}
  void disablePing() override {}

  bool closed{false};
  drogon::CloseCode closeCode{drogon::CloseCode::kNormalClosure};
  std::string closeReason;

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};

std::string revocation(int64_t user, std::string_view session)
{
  Json::Value change(Json::objectValue);
  change[sync_change::kActionField] = sync_change::kActionDisconnectSession;
  change[sync_change::kUserField] = static_cast<Json::Int64>(user);
  change[sync_change::kSessionField] = std::string(session);
  return json_util::toString(change);
}
}

TEST_CASE("a disconnect_session change names the user and the session it ends")
{
  const auto key = session_revocation::parse(revocation(7, "aa11"));
  REQUIRE(key.has_value());
  CHECK(key.value_or(MediaSessionKey{}).userId == 7);
  CHECK(key.value_or(MediaSessionKey{}).sessionId == "aa11");
}

TEST_CASE("any other change, or a malformed revocation, names no session")
{
  CHECK_FALSE(session_revocation::parse(R"({"action":"emit","users":[7]})").has_value());
  CHECK_FALSE(session_revocation::parse("not json").has_value());
  CHECK_FALSE(session_revocation::parse(revocation(0, "aa11")).has_value());
  CHECK_FALSE(session_revocation::parse(revocation(7, "")).has_value());
  CHECK_FALSE(session_revocation::parse(revocation(7, std::string(65, 'a'))).has_value());
  CHECK_FALSE(session_revocation::parse(
                  R"({"action":"disconnect_session","user":"7","session":"aa11"})")
                  .has_value());
}

TEST_CASE("only the revoked session's live views close")
{
  MediaSessionRegistry registry;
  const auto revoked = std::make_shared<ClosingConnection>();
  const auto revokedTwin = std::make_shared<ClosingConnection>();
  const auto otherSession = std::make_shared<ClosingConnection>();
  const auto otherUser = std::make_shared<ClosingConnection>();
  registry.add({.connection = revoked, .session = {.userId = 7, .sessionId = "aa11"}});
  registry.add({.connection = revokedTwin, .session = {.userId = 7, .sessionId = "aa11"}});
  registry.add({.connection = otherSession, .session = {.userId = 7, .sessionId = "bb22"}});
  registry.add({.connection = otherUser, .session = {.userId = 8, .sessionId = "aa11"}});
  REQUIRE(registry.size() == 4);

  CHECK(registry.closeSession({.userId = 7, .sessionId = "aa11"}) == 2);
  CHECK(revoked->closed);
  CHECK(revokedTwin->closed);
  CHECK(revoked->closeCode == drogon::CloseCode::kViolation);
  CHECK(revoked->closeReason == "session_revoked");
  CHECK_FALSE(otherSession->closed);
  CHECK_FALSE(otherUser->closed);
  CHECK(registry.size() == 2);

  CHECK(registry.closeSession({.userId = 7, .sessionId = "aa11"}) == 0);
}

TEST_CASE("a closed socket leaves the registry, and one with no session is never kept")
{
  MediaSessionRegistry registry;
  const auto live = std::make_shared<ClosingConnection>();
  const auto anonymous = std::make_shared<ClosingConnection>();
  registry.add({.connection = live, .session = {.userId = 7, .sessionId = "aa11"}});
  registry.add({.connection = anonymous, .session = {.userId = 7, .sessionId = ""}});
  CHECK(registry.size() == 1);

  registry.remove(live);
  CHECK(registry.size() == 0);
  CHECK(registry.closeSession({.userId = 7, .sessionId = "aa11"}) == 0);
  CHECK_FALSE(live->closed);
}

namespace
{
std::string userAudit(int64_t user, const std::string& changes)
{
  return R"({"kind":"audit","record_id":)" + std::to_string(user) +
         R"(,"table_name":"user","changes":)" + changes +
         R"(,"priority":1,"users":[1],"event_timestamp":1})";
}
}

TEST_CASE("a role or account change of a user names that user, any other change names nobody")
{
  CHECK(session_revocation::parseUserChange(
            userAudit(7, R"({"role":{"previous":"resident","current":"guest"}})")) ==
        std::optional<int64_t>(7));
  CHECK(session_revocation::parseUserChange(
            userAudit(9, R"({"isActive":{"previous":true,"current":false}})")) ==
        std::optional<int64_t>(9));
  CHECK_FALSE(session_revocation::parseUserChange(
                  userAudit(7, R"({"name":{"previous":"Ana","current":"Ana M"}})"))
                  .has_value());
  CHECK_FALSE(session_revocation::parseUserChange(
                  R"({"kind":"audit","record_id":7,"table_name":"person","changes":{"role":{}}})")
                  .has_value());
  CHECK_FALSE(session_revocation::parseUserChange(
                  R"({"kind":"identity","table":"user","id":7,"row":{"role":"guest"}})")
                  .has_value());
  CHECK_FALSE(session_revocation::parseUserChange(userAudit(0, R"({"role":{}})")).has_value());
  CHECK_FALSE(session_revocation::parseUserChange("not json").has_value());
}

TEST_CASE("a role change closes every live view of that user, and only theirs")
{
  MediaSessionRegistry registry;
  const auto first = std::make_shared<ClosingConnection>();
  const auto second = std::make_shared<ClosingConnection>();
  const auto other = std::make_shared<ClosingConnection>();
  registry.add({.connection = first, .session = {.userId = 7, .sessionId = "aa11"}});
  registry.add({.connection = second, .session = {.userId = 7, .sessionId = "bb22"}});
  registry.add({.connection = other, .session = {.userId = 8, .sessionId = "aa11"}});

  CHECK(registry.closeUser(7) == 2);
  CHECK(first->closed);
  CHECK(second->closed);
  CHECK(first->closeReason == "role_changed");
  CHECK_FALSE(other->closed);
  CHECK(registry.size() == 1);

  CHECK(registry.closeAll("module_disabled") == 1);
  CHECK(other->closed);
  CHECK(other->closeReason == "module_disabled");
  CHECK(registry.size() == 0);
}
