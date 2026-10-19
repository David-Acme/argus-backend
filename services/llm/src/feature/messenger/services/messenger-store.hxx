#pragma once

#include <functional>

struct sqlite3;

class MessengerStore
{
public:
  MessengerStore() = default;
  virtual ~MessengerStore() = default;
  MessengerStore(const MessengerStore&) = delete;
  MessengerStore& operator=(const MessengerStore&) = delete;

  virtual void run(const std::function<void(sqlite3*)>& body) const = 0;
};
