#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace tunnel::link
{
std::optional<std::string> subjectPublicKeyDerOfPem(std::string_view pem);
std::optional<std::string> subjectPublicKeyDerOfFile(const std::string& path);
}
