#pragma once

#include <feature/modules/schemas/module-view.hxx>
#include <json/value.h>

#include <string_view>

namespace module_json
{
[[nodiscard]] Json::Value job(const JobView& view);
[[nodiscard]] Json::Value module(const ModuleView& view, std::string_view lang);
[[nodiscard]] Json::Value member(const ModuleView& view, std::string_view lang);
[[nodiscard]] Json::Value enabledModules(const ModuleStatesReply& set);
}
