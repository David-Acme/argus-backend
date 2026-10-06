#pragma once

#include "decider.hxx"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace turn
{

enum class Awaiting : unsigned char
{
  Slot,
  Approval,
  Choice
};

struct Pending
{
  Awaiting awaiting{Awaiting::Slot};
  Candidate candidate;
  std::optional<Candidate> alternative;
  std::string slot;
  std::string utterance{};
  int attempts{0};
  std::chrono::steady_clock::time_point at{};
};

class PendingTurns
{
public:
  static constexpr std::chrono::minutes kLife{5};

  void put(int64_t userId, Pending pending);

  [[nodiscard]] std::optional<Pending> peek(int64_t userId) const;

  void forget(int64_t userId);

private:
  mutable std::mutex mutex_;
  mutable std::map<int64_t, Pending> pendings_;
};

}
