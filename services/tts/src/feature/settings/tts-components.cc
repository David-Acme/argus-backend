#include "tts-components.hxx"

#include <string>
#include <utility>

DiskComponentHostInput ttsComponents(TtsComponentsInput input)
{
  return {.modelsDir = std::move(input.modelsDir),
          .owned = {std::string(kTtsComponent)},
          .fetch = {},
          .ready = [loaded = std::move(input.loaded)](const std::string&) { return loaded && loaded(); }};
}
