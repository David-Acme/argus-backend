#include "llm-components.hxx"

#include <string>
#include <utility>

DiskComponentHostInput llmComponents(LlmComponentsInput input)
{
  return {.modelsDir = std::move(input.modelsDir),
          .owned = {std::string(kLlmComponent)},
          .fetch = {},
          .ready = [loaded = std::move(input.loaded)](const std::string&) { return loaded && loaded(); }};
}
