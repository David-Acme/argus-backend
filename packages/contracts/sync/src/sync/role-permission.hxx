#pragma once

#include <cstdint>

// What a role must hold to touch one synced table: the third element of the
// gate's (role, table, permission) triple and the requirement a tool descriptor
// declares. Declared once, here, because it is only meaningful beside TableName
// -- a permission names no table of its own -- and because the units that read
// it are three tiers apart: the tier-4 auth lib owns the role table, the tier-3
// llm client declares tool descriptors, and the two services that gate a route
// by hand all need the same four names.
//
// No toString/fromString pair, unlike TableName: the permission never crosses
// the wire, so nothing parses it back and the gate compares the enum directly.
enum class RolePermission : uint8_t
{
  Read = 0,
  Create,
  Update,
  Delete
};
