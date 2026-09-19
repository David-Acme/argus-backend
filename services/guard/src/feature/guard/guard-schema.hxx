#pragma once

#include <string>

namespace guard_schema
{

// Rebuilds pre-v2 guard tables in place; idempotent and row preserving.
bool migrate(const std::string& schemaPath);

} // namespace guard_schema
