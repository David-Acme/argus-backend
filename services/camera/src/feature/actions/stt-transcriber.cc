#include "stt-transcriber.hxx"

#include <stt/stt-remote.hxx>

namespace
{
class HttpSttTranscriber final : public SttTranscriber
{
public:
  std::string transcribe(const std::vector<int16_t>& samples,
                         const std::string& lang) const override
  {
    if (!client_.remote())
      return {};
    std::vector<float> floats;
    floats.reserve(samples.size());
    for (const int16_t sample : samples)
      floats.push_back(static_cast<float>(sample) / kPcmScale);
    return client_.transcribe(floats, lang);
  }

private:
  SttClient client_;
};
}

std::unique_ptr<SttTranscriber> makeHttpSttTranscriber()
{
  return std::make_unique<HttpSttTranscriber>();
}
