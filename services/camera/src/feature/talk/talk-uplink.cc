#include "talk-uplink.hxx"

#include <algorithm>

TalkUplink::TalkUplink(TalkUplinkConfig config)
    : config_(config),
      resampler_({.sourceRate = config.sourceRate, .targetRate = kLineRate}),
      packetSamples_(static_cast<size_t>(kLineRate * config.packetMs / 1000)),
      maxQueued_(static_cast<size_t>(kLineRate * config.maxQueuedMs / 1000))
{
}

void TalkUplink::push(std::span<const int16_t> samples)
{
  if (samples.empty())
    return;
  const std::vector<int16_t> line = config_.sourceRate == kLineRate
                                        ? std::vector<int16_t>(samples.begin(), samples.end())
                                        : resampler_.process(samples.data(), samples.size());
  queue_.insert(queue_.end(), line.begin(), line.end());
  if (queue_.size() > maxQueued_) {
    const size_t excess = queue_.size() - maxQueued_;
    queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(excess));
    droppedSamples_ += static_cast<int64_t>(excess);
  }
}

std::optional<std::vector<int16_t>> TalkUplink::next()
{
  if (queue_.size() < packetSamples_)
    return std::nullopt;
  std::vector<int16_t> packet(queue_.begin(),
                              queue_.begin() + static_cast<std::ptrdiff_t>(packetSamples_));
  queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(packetSamples_));
  return packet;
}

int TalkUplink::queuedMs() const
{
  return static_cast<int>(queue_.size() * 1000 / kLineRate);
}

std::optional<std::vector<int16_t>> talk_frame::parse(std::span<const uint8_t> frame)
{
  if (frame.size() < kHeaderBytes || frame.size() > kMaxFrameBytes || frame[0] != kMagic ||
      frame[1] != kAudio)
    return std::nullopt;
  const auto payload = frame.subspan(kHeaderBytes);
  if (payload.size() % 2 != 0)
    return std::nullopt;
  std::vector<int16_t> samples(payload.size() / 2);
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto low = static_cast<uint16_t>(payload[2 * i]);
    const auto high = static_cast<uint16_t>(payload[(2 * i) + 1]);
    samples[i] = static_cast<int16_t>(static_cast<uint16_t>(low | static_cast<uint16_t>(high << 8U)));
  }
  return samples;
}
