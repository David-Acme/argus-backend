#pragma once

#include <cstdint>

enum class RolePermission : uint8_t
{
  Read = 0,
  Create,
  Update,
  Delete
};
