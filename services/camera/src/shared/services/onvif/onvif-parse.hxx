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
std::string targetOf(std::string_view address);
std::optional<int64_t> epochOf(std::string_view dateTime);
std::optional<OnvifSubscription> subscription(const pugi::xml_node& response);
OnvifPull pulled(const pugi::xml_node& response);
std::optional<bool> publishesMotion(const pugi::xml_node& response);
int64_t lifetimeOf(const pugi::xml_node& response);
}
