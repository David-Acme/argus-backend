#pragma once

#include <auth/jwt-filter.hxx>
#include <cstdint>
#include <memory>
#include <sync/syncable.hxx>

enum class IdentitySyncTable : std::uint8_t
{
  User,
  UserInvitation,
  Person,
};

class IdentitySyncSource
{
public:
  virtual ~IdentitySyncSource() = default;

  [[nodiscard]] virtual bool serves(IdentitySyncTable table) const = 0;

  [[nodiscard]] virtual std::unique_ptr<Syncable>
  sourceFor(IdentitySyncTable table, const JwtContext& ctx) const = 0;
};
