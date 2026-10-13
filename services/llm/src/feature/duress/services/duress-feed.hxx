#pragma once

#include <string_view>

namespace duress_feed
{

[[nodiscard]] bool changesPhrase(std::string_view payload);

}
