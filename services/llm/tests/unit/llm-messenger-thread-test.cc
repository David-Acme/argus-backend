#include <doctest/doctest.h>

#include <feature/messenger/repositories/relay-thread/relay-thread-repository.hxx>
#include <feature/messenger/vocabulary/relay-thread-state.hxx>

#include <sqlite3.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace
{

constexpr int64_t kWindow = 300;
constexpr int64_t kStart = 1000;

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

Database openSchema()
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(":memory:", &raw) == SQLITE_OK);
  Database db(raw, &sqlite3_close);
  std::ifstream in(ARGUS_TEST_LLM_SCHEMA);
  REQUIRE(in.good());
  std::stringstream sql;
  sql << in.rdbuf();
  char* error = nullptr;
  const int rc = sqlite3_exec(db.get(), sql.str().c_str(), nullptr, nullptr, &error);
  if (error != nullptr)
    sqlite3_free(error);
  REQUIRE(rc == SQLITE_OK);
  return db;
}

int64_t relayAt(sqlite3* db, int64_t from, int64_t to, const std::string& messageId, int64_t at)
{
  const RelayThreadRepository repository;
  return repository.create(db, {.fromUserId = from,
                                .toUserId = to,
                                .messageId = messageId,
                                .channel = RelayChannel::Notification,
                                .replyUntil = at + kWindow,
                                .at = at});
}

}

TEST_CASE("the thread states round-trip through their wire names and an unknown one is refused")
{
  for (const auto state : {RelayThreadState::Open, RelayThreadState::Answered, RelayThreadState::Expired}) {
    REQUIRE(relayThreadStateFromString(relayThreadStateToString(state)).has_value());
    CHECK(relayThreadStateFromString(relayThreadStateToString(state)).value() == state);
  }
  CHECK_FALSE(relayThreadStateFromString("closed").has_value());
}

TEST_CASE("a relay opens a thread the recipient can answer inside the reply window, with the channel kept")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  const int64_t id = relayAt(db.get(), 1, 2, "n-7", kStart);
  CHECK(id > 0);
  const auto open = repository.openFor(db.get(), {.toUserId = 2, .now = kStart + 100});
  REQUIRE(open.has_value());
  CHECK(open->id == id);
  CHECK(open->fromUserId == 1);
  CHECK(open->toUserId == 2);
  CHECK(open->messageId == "n-7");
  CHECK(open->channel == RelayChannel::Notification);
  CHECK(open->state == RelayThreadState::Open);
}

TEST_CASE("a reply after the window is not a reply to the thread")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  relayAt(db.get(), 1, 2, "n-8", kStart);
  CHECK_FALSE(repository.openFor(db.get(), {.toUserId = 2, .now = kStart + kWindow + 1}).has_value());
}

TEST_CASE("a thread is only open for the member it was sent to")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  relayAt(db.get(), 1, 2, "n-9", kStart);
  CHECK_FALSE(repository.openFor(db.get(), {.toUserId = 3, .now = kStart}).has_value());
  CHECK_FALSE(repository.openFor(db.get(), {.toUserId = 1, .now = kStart}).has_value());
}

TEST_CASE("the newest open thread for a member is the one a reply travels on")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  relayAt(db.get(), 1, 2, "older", kStart);
  const int64_t newer = relayAt(db.get(), 3, 2, "newer", kStart + 10);
  const auto open = repository.openFor(db.get(), {.toUserId = 2, .now = kStart + 20});
  REQUIRE(open.has_value());
  CHECK(open->id == newer);
  CHECK(open->fromUserId == 3);
}

TEST_CASE("an answer closes a thread once, and a second answer is refused")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  const int64_t id = relayAt(db.get(), 1, 2, "n-10", kStart);
  CHECK(repository.answer(db.get(), id));
  CHECK_FALSE(repository.answer(db.get(), id));
  CHECK_FALSE(repository.openFor(db.get(), {.toUserId = 2, .now = kStart}).has_value());
}

TEST_CASE("threads past their window expire and stop counting as open")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  relayAt(db.get(), 1, 2, "n-11", kStart);
  const int64_t answered = relayAt(db.get(), 1, 4, "n-12", kStart);
  CHECK(repository.answer(db.get(), answered));
  const auto expired = repository.expire(db.get(), kStart + kWindow + 1);
  REQUIRE(expired.size() == 1);
  CHECK(expired.front() == "n-11");
  CHECK(repository.expire(db.get(), kStart + kWindow + 1).empty());
}

TEST_CASE("a delivered thread records its channel and the inbox key of its message")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  const int64_t id = relayAt(db.get(), 1, 2, "pending", kStart);
  CHECK(repository.setDelivery(db.get(), {.id = id, .channel = RelayChannel::Conversation, .messageId = "relay-1-9"}));
  const auto open = repository.openFor(db.get(), {.toUserId = 2, .now = kStart});
  REQUIRE(open.has_value());
  CHECK(open->channel == RelayChannel::Conversation);
  CHECK(open->messageId == "relay-1-9");
}

TEST_CASE("the relays a sender made since a time are counted, and older ones and other senders' are not")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  relayAt(db.get(), 1, 2, "c-1", kStart);
  relayAt(db.get(), 1, 3, "c-2", kStart + 100);
  relayAt(db.get(), 4, 2, "c-3", kStart + 100);
  CHECK(repository.countFrom(db.get(), {.fromUserId = 1, .since = kStart}) == 2);
  CHECK(repository.countFrom(db.get(), {.fromUserId = 1, .since = kStart + 50}) == 1);
  CHECK(repository.countFrom(db.get(), {.fromUserId = 9, .since = kStart}) == 0);
}

TEST_CASE("settled threads older than the cut-off are purged and an open thread is kept")
{
  auto db = openSchema();
  const RelayThreadRepository repository;
  const int64_t answered = relayAt(db.get(), 1, 2, "n-13", kStart);
  relayAt(db.get(), 1, 3, "n-14", kStart);
  repository.answer(db.get(), answered);
  static_cast<void>(repository.expire(db.get(), kStart + kWindow + 1));
  const int64_t kept = repository.create(db.get(), {.fromUserId = 1,
                                                    .toUserId = 5,
                                                    .messageId = "n-15",
                                                    .channel = RelayChannel::Conversation,
                                                    .replyUntil = kStart + 10 * kWindow,
                                                    .at = kStart});
  CHECK(repository.purge(db.get(), kStart + kWindow + 1) == 2);
  const auto open = repository.openFor(db.get(), {.toUserId = 5, .now = kStart + 1});
  REQUIRE(open.has_value());
  CHECK(open->id == kept);
  CHECK(open->channel == RelayChannel::Conversation);
}
