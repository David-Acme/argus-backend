#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace turn
{

struct DecisionKey
{
  std::string decider;
  bool exact{false};
  std::string family;
  std::string lang;
  std::string verdict;

  auto operator<=>(const DecisionKey&) const = default;
};

struct DecisionCount
{
  DecisionKey key;
  std::uint64_t count{0};
};

struct Decision
{
  std::string_view decider;
  bool exact{false};
  std::string_view tool;
  std::string_view lang;
  std::string_view verdict;
};

class DecisionTally
{
public:
  void note(const Decision& decision);

  [[nodiscard]] std::vector<DecisionCount> snapshot() const;

private:
  mutable std::mutex mutex_;
  std::map<DecisionKey, std::uint64_t> counts_;
};

}
