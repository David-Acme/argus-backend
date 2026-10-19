#pragma once

#include <feature/messenger/services/messenger-store.hxx>

#include <functional>
#include <mutex>

struct sqlite3;

class DirectMessengerStore final : public MessengerStore
{
public:
  explicit DirectMessengerStore(sqlite3* db)
      : db_(db)
  {
  }

  void run(const std::function<void(sqlite3*)>& body) const override
  {
    const std::scoped_lock lock(mutex_);
    body(db_);
  }

private:
  sqlite3* db_;
  mutable std::mutex mutex_;
};
