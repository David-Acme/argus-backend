#include "wake-spotter.hxx"

#include <stt/stt-remote.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{

class SttWakeStream final : public IWakeStream
{
public:
  explicit SttWakeStream(std::unique_ptr<argus::stt::SpotStream> stream) : stream_(std::move(stream)) {}

  bool push(std::span<const float> samples) override { return stream_->push(samples); }

  bool alive() const override { return stream_->alive(); }

private:
  std::unique_ptr<argus::stt::SpotStream> stream_;
};

}

std::unique_ptr<IWakeStream> SttWakeSpotter::open(std::function<void(const WakeHit&)> onHit)
{
  try {
    const SttClient client;
    auto stream = client.openSpot({.sampleRate = kWireSampleRate,
                                   .onHit = [onHit = std::move(onHit)](const argus::stt::SpotHitEvent& hit) {
                                     if (onHit)
                                       onHit({.keyword = hit.keyword});
                                   }});
    if (!stream)
      return nullptr;
    return std::make_unique<SttWakeStream>(std::move(stream));
  }
  catch (const std::exception& error) {
    LOG_WARN << "Voice watch: the keyword spotter stream did not open: " << error.what();
  }
  return nullptr;
}
