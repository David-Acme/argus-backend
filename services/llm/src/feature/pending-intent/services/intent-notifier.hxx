#pragma once

#include <cstdint>
#include <string>

struct IntentNotice
{
  int64_t userId{0};
  std::string title;
  std::string body;
  std::string commandId;
};

class IntentNotifier
{
public:
  IntentNotifier() = default;
  virtual ~IntentNotifier() = default;
  IntentNotifier(const IntentNotifier&) = delete;
  IntentNotifier& operator=(const IntentNotifier&) = delete;

  [[nodiscard]] virtual bool tell(const IntentNotice& notice) const = 0;
};
