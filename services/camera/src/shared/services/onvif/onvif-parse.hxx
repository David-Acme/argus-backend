#pragma once

#include <shared/services/onvif/onvif-types.hxx>

#include <cstdint>
#include <optional>
#include <pugixml.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace onvif_parse
{
std::optional<int64_t> deviceEpoch(const pugi::xml_node& response);
OnvifServices services(const pugi::xml_node& response);
OnvifDeviceInfo deviceInfo(const pugi::xml_node& response);
std::vector<OnvifProfile> profiles(const pugi::xml_node& response);
std::vector<OnvifPreset> presets(const pugi::xml_node& response);
std::string presetToken(const pugi::xml_node& response);
std::string pathOf(std::string_view xaddr);
}
