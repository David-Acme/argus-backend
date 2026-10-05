#pragma once

#include <feature/presence/services/presence-ports.hxx>

class IdentityClient;

class IdentityPresenceDirectory final : public PresenceDirectory
{
public:
  explicit IdentityPresenceDirectory(const IdentityClient* identity);

  [[nodiscard]] std::optional<bool>
  presenceConsent(int64_t userId) const override;

  [[nodiscard]] std::optional<int64_t>
  userOfPerson(int64_t personId) const override;

  [[nodiscard]] std::optional<std::vector<int64_t>>
  consentingUsers() const override;

private:
  const IdentityClient* identity_{nullptr};
};
