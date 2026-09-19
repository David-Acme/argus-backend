#include "stt-transcriber.hxx"

#include <shared/services/stt/remote/stt-remote.hxx>
#include <utility>

namespace
{
class HttpSttTranscriber final : public SttTranscriber
{
public:
  std::string transcribe(const std::vector<int16_t>& samples,
                         const std::string& lang) const override
  {
    const SttRemoteConfig config = SttRemoteConfig::resolve();
    if (!config.enabled())
      return {};
    std::vector<float> floats;
    floats.reserve(samples.size());
    for (const int16_t sample : samples)
      floats.push_back(static_cast<float>(sample) / kPcmScale);
    return SttHttpClient(config.url, config.timeoutMs).transcribe(floats, lang);
  }
};
} // namespace

std::unique_ptr<SttTranscriber> makeHttpSttTranscriber()
{
  return std::make_unique<HttpSttTranscriber>();
}
