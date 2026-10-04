#pragma once

#include <feature/call/services/call-ports.hxx>
#include <identity/identity-client.hxx>

#include <memory>

class IdentityCallDirectory final : public CallDirectory
{
public:
  explicit IdentityCallDirectory(std::shared_ptr<const IdentityClient> client);

  [[nodiscard]] CallRecipient recipient(int64_t userId) const override;

  [[nodiscard]] CallPerson person(int64_t personId) const override;

private:
  std::shared_ptr<const IdentityClient> client_;
};
