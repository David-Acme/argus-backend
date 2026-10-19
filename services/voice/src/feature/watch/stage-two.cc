#include "stage-two.hxx"

#include <trantor/utils/Logger.h>

StageTwoResult judgeWake(const std::string& transcript, int maxDistance)
{
  StageTwoResult result;
  result.transcribed = true;
  result.transcript = transcript;
  result.match = matchWake({.text = transcript, .maxDistance = maxDistance});
  result.confidence = wakeConfidence(result.match);
  result.accepted = result.match.position == WakePosition::Vocative;
  return result;
}

StageTwoResult StageTwo::confirm(const StageTwoInput& input) const
{
  if (input.samples.empty())
    return {};
  try {
    const std::string transcript =
        stt_.transcribe({.samples = input.samples, .sampleRate = 16000, .language = input.language});
    return judgeWake(transcript, input.maxDistance);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Voice watch: stage two could not transcribe the wake: " << error.what();
  }
  return {};
}
