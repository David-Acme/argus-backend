#pragma once

#include <feature/messenger/services/household-roster.hxx>
#include <identity/identity-client.hxx>

#include <memory>

class IdentityHouseholdRoster final : public HouseholdRoster
{
public:
  explicit IdentityHouseholdRoster(std::shared_ptr<const IdentityClient> client);

  [[nodiscard]] std::vector<relay_recipient::Member> members() const override;

private:
  std::shared_ptr<const IdentityClient> client_;
};
