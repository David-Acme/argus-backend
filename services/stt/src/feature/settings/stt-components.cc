#include "stt-components.hxx"

#include <string>
#include <utility>

DiskComponentHostInput sttComponents(SttComponentsInput input)
{
  return {.modelsDir = std::move(input.modelsDir),
          .owned = {std::string(kSttComponent)},
          .fetch = {},
          .ready = [loaded = std::move(input.loaded)](const std::string&) { return loaded && loaded(); }};
}
