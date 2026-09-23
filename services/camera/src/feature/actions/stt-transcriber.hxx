#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class SttTranscriber
{
public:
  virtual ~SttTranscriber() = default;

  virtual std::string transcribe(const std::vector<int16_t>& samples,
                                 const std::string& lang) const = 0;
};

std::unique_ptr<SttTranscriber> makeHttpSttTranscriber();
