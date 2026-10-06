#pragma once

#include <auth/module-snapshot.hxx>
#include <auth/user-role.hxx>

#include <string>
#include <string_view>

namespace role_copy
{
[[nodiscard]] std::string label(UserRole role, std::string_view lang);
[[nodiscard]] std::string needsModule(UserRole role, const ModuleFlag& module, std::string_view lang);
}
