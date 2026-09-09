#include "tool-validator.hxx"

namespace tools
{

std::optional<std::string> validateArguments(const ToolDescriptor& descriptor,
                                             const ToolCall& call)
{
  if (!call.arguments.isObject())
    return "arguments must be a JSON object";

  for (const auto& spec : descriptor.arguments) {
    const bool present = call.arguments.isMember(spec.name);
    if (spec.required && !present)
      return "missing required argument '" + spec.name + "'";
    if (!present)
      continue;
    const Json::Value& value = call.arguments[spec.name];
    if (spec.type == "string" && !value.isString())
      return "argument '" + spec.name + "' must be a string";
    if (spec.type == "number" && !value.isNumeric())
      return "argument '" + spec.name + "' must be a number";
    if (spec.type == "boolean" && !value.isBool())
      return "argument '" + spec.name + "' must be a boolean";
    if (spec.type == "enum") {
      bool known = false;
      if (value.isString()) {
        const std::string raw = value.asString();
        for (const auto& allowed : spec.enumValues) {
          if (raw == allowed) {
            known = true;
            break;
          }
        }
      }
      if (!known)
        return "argument '" + spec.name + "' has an invalid value";
    }
  }
  return std::nullopt;
}

} // namespace tools
