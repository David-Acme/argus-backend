#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Transcribes 16 kHz mono s16 PCM; the production implementation calls
// argus-stt, tests inject a fake through the service composition root.
class SttTranscriber
{
public:
  virtual ~SttTranscriber() = default;

  virtual std::string transcribe(const std::vector<int16_t>& samples,
                                 const std::string& lang) const = 0;
};

// Production transcriber over the argus-stt internal wire; empty when the
// remote URL is unset.
std::unique_ptr<SttTranscriber> makeHttpSttTranscriber();
