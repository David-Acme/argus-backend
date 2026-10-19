#pragma once

#include <feature/messenger/services/relay-recipient.hxx>

#include <vector>

class HouseholdRoster
{
public:
  HouseholdRoster() = default;
  virtual ~HouseholdRoster() = default;
  HouseholdRoster(const HouseholdRoster&) = delete;
  HouseholdRoster& operator=(const HouseholdRoster&) = delete;

  [[nodiscard]] virtual std::vector<relay_recipient::Member> members() const = 0;
};
