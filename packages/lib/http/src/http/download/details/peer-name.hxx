#pragma once

#include <string>
#include <string_view>

namespace file_download::details
{

struct PeerNameInput
{
  std::string_view certificatePem;
  std::string_view origin;
};

[[nodiscard]] std::string hostOfOrigin(std::string_view origin);

[[nodiscard]] bool certificateNamesHost(const PeerNameInput& input);

}
