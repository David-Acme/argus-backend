#pragma once

#include <feature/guard/services/response-plan.hxx>

class IdentityClient;

class IdentityResponseDirectory final : public ResponseDirectory
{
public:
  explicit IdentityResponseDirectory(const IdentityClient* identity);

  [[nodiscard]] std::optional<std::vector<ResponseUser>> users() const override;

private:
  const IdentityClient* identity_;
};
