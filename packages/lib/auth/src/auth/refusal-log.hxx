#pragma once

#include <string>
#include <string_view>

namespace auth_refusal
{
std::string_view reasonOfVerdict(std::string_view verdictReason);

std::string line(std::string_view path, int status, std::string_view reason);
}
