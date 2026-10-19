#include "camera-ear.hxx"

#include <feature/watch/wake-words.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <utility>

namespace
{

constexpr auto kFireSourceKws = std::string_view("kws");
constexpr auto kFireSourceStt = std::string_view("stt");

size_t samplesOf(std::chrono::milliseconds duration)
{
  return static_cast<size_t>(duration.count()) * static_cast<size_t>(kWatchRate) / 1000;
}

std::vector<float> floatsOf(const std::string& pcm)
{
  std::vector<float> samples(pcm.size() / 2);
  for (size_t index = 0; index < samples.size(); ++index) {
    const auto low = static_cast<uint16_t>(static_cast<unsigned char>(pcm[2 * index]));
    const auto high = static_cast<uint16_t>(static_cast<unsigned char>(pcm[(2 * index) + 1]));
    const auto value = static_cast<int16_t>(static_cast<uint16_t>(low | static_cast<uint16_t>(high << 8U)));
    samples[index] = static_cast<float>(value) / 32768.0F;
  }
  return samples;
}

}

void WakeCapture::append(std::span<const float> frame)
{
  {
    std::scoped_lock lock(mutex);
    if (complete)
      return;
    const size_t room = wanted > samples.size() ? wanted - samples.size() : 0;
    samples.insert(samples.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(std::min(room, frame.size())));
    complete = samples.size() >= wanted;
  }
  done.notify_all();
}

bool WakeCapture::wait(std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex);
  return done.wait_for(lock, timeout, [this] { return complete; });
}

std::vector<float> WakeCapture::snapshot()
{
  std::scoped_lock lock(mutex);
  return samples;
}

CameraEar::CameraEar(CameraEarInput input)
    : input_(std::move(input)),
      ring_(samplesOf(input_.config.ring)),
      level_({.floorRiseDbPerSecond = input_.config.floorRiseDbPerSecond,
              .floorFallDbPerSecond = input_.config.floorFallDbPerSecond,
              .initialFloorDb = -60.0F,
              .window = input_.config.ring,
              .sampleRate = kWatchRate}),
      vad_(input_.vad != nullptr ? input_.vad->createModel() : makeSileroVadModel())
{
}

CameraEar::~CameraEar()
{
  stop();
}

void CameraEar::start()
{
  reader_ = std::jthread([this](const std::stop_token& stop) { readLoop(stop); });
}

void CameraEar::stop()
{
  if (reader_.joinable()) {
    reader_.request_stop();
    reader_.join();
  }
  spot_.reset();
  if (segmentThread_.joinable())
    segmentThread_.join();
}

float CameraEar::snrDb() const
{
  std::scoped_lock lock(mutex_);
  return level_.speechSnrDb();
}

void CameraEar::readLoop(const std::stop_token& stop)
{
  std::mutex waitMutex;
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    listening_.store(true);
    const WatchMicrophoneEnd end = input_.camera->readMicrophone(
        {.cameraId = input_.cameraId,
         .onFrame = [this](const argus::camera::v1::MicrophoneFrame& frame) { onPcm(frame); },
         .stop = stop});
    listening_.store(false);
    if (end.stopped || stop.stop_requested())
      return;
    LOG_INFO << "Voice watch: camera " << input_.cameraId << " microphone closed ("
             << argus::camera::v1::WatchRefusal_Name(end.refusal) << ", " << end.status.error_message()
             << "); retrying";
    std::unique_lock lock(waitMutex);
    static_cast<void>(wake.wait_for(lock, stop, input_.config.reconnect, [] { return false; }));
  }
}

std::vector<float> CameraEar::ringTail(size_t samples)
{
  std::vector<float> all;
  {
    std::scoped_lock lock(mutex_);
    ring_.peekInto(all);
  }
  if (all.size() > samples)
    all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(samples));
  return all;
}

void CameraEar::onPcm(const argus::camera::v1::MicrophoneFrame& frame)
{
  const std::vector<float> samples = floatsOf(frame.pcm());
  if (samples.empty())
    return;
  const auto now = WatchClock::now();
  static_cast<void>(vad_.process({.samples = samples.data(), .count = static_cast<int>(samples.size())}));
  const bool speech = vad_.inSpeech();
  float db = -120.0F;
  float snr = 0.0F;
  {
    std::scoped_lock lock(mutex_);
    ring_.push(samples.data(), samples.size());
    level_.observe(samples, speech);
    db = level_.lastDb();
    snr = level_.speechSnrDb();
    for (const auto& capture : captures_)
      capture->append(samples);
    std::erase_if(captures_, [](const std::shared_ptr<WakeCapture>& capture) {
      std::scoped_lock captureLock(capture->mutex);
      return capture->complete;
    });
  }
  speech_.store(speech);
  if (speech)
    lastSpeechAt_ = now;
  feedSpotter(samples, speech, now);
  watchSttSegment(speech, now);
  if (input_.onFrame)
    input_.onFrame({.cameraId = input_.cameraId,
                    .samples = samples,
                    .speech = speech,
                    .db = db,
                    .snrDb = snr,
                    .at = now,
                    .gap = frame.gap()});
}

void CameraEar::feedSpotter(std::span<const float> frame, bool speech, WatchClock::time_point now)
{
  if (input_.config.stageOne == WakeStageOne::Stt || input_.spotter == nullptr)
    return;
  if ((!spot_ || !spot_->alive()) && now >= spotRetryAt_) {
    spotRetryAt_ = now + input_.config.kwsRetry;
    spot_ = input_.spotter->open([this](const WakeHit&) { fire(kFireSourceKws, std::nullopt); });
    if (spot_ && !kwsSeen_)
      LOG_INFO << "Voice watch: camera " << input_.cameraId << " listens for the wake word with the keyword spotter";
  }
  const bool feeding = speech || now - lastSpeechAt_ < input_.config.kwsHangover;
  if (spot_ && feeding && spot_->push(frame))
    kwsSeen_ = true;
}

void CameraEar::watchSttSegment(bool speech, WatchClock::time_point now)
{
  const bool kwsAlive = spot_ && spot_->alive();
  const bool useStt = input_.config.stageOne == WakeStageOne::Stt ||
                      (input_.config.stageOne == WakeStageOne::Auto && !kwsAlive);
  if (!useStt || input_.stt == nullptr) {
    inSegment_ = false;
    return;
  }
  if (speech && !inSegment_) {
    inSegment_ = true;
    segmentSent_ = false;
    segmentStart_ = now;
  }
  if (!inSegment_)
    return;
  const auto length = std::chrono::duration_cast<std::chrono::milliseconds>(now - segmentStart_);
  const bool ended = !speech;
  if (!segmentSent_ && (length >= input_.config.sttStageOneMax || (ended && length >= input_.config.sttStageOneMin))) {
    segmentSent_ = true;
    if (snrDb() >= input_.config.sttStageOneMinSnrDb && !transcribing_.load()) {
      const auto span = std::min(length + input_.config.kwsHangover, input_.config.ring);
      transcribeSegment(ringTail(samplesOf(span)));
    }
  }
  if (ended)
    inSegment_ = false;
}

void CameraEar::transcribeSegment(std::vector<float> segment)
{
  if (segment.empty())
    return;
  if (segmentThread_.joinable())
    segmentThread_.join();
  transcribing_.store(true);
  segmentThread_ = std::jthread([this, segment = std::move(segment)] {
    try {
      const std::string text =
          input_.stt->transcribe({.samples = segment, .sampleRate = kWatchRate, .language = input_.language});
      if (mentionsWake({.text = text, .maxDistance = input_.config.wakeMaxDistance}))
        fire(kFireSourceStt, text);
    }
    catch (const std::exception& error) {
      LOG_DEBUG << "Voice watch: a speech segment was not transcribed: " << error.what();
    }
    transcribing_.store(false);
  });
}

void CameraEar::fire(std::string_view source, std::optional<std::string> transcript)
{
  EarFire out;
  {
    std::scoped_lock lock(mutex_);
    const auto now = WatchClock::now();
    if (lastFireAt_ != WatchClock::time_point{} && now - lastFireAt_ < input_.config.fireDebounce)
      return;
    lastFireAt_ = now;
    std::vector<float> ring;
    ring_.peekInto(ring);
    auto capture = std::make_shared<WakeCapture>();
    capture->start = now - std::chrono::milliseconds(static_cast<int64_t>(ring.size()) * 1000 / kWatchRate);
    capture->wanted = transcript ? ring.size() : ring.size() + samplesOf(input_.config.postRoll);
    capture->complete = transcript.has_value();
    out = EarFire{.cameraId = input_.cameraId,
                  .at = now,
                  .source = source,
                  .snrDb = level_.speechSnrDb(),
                  .floorDb = level_.floorDb(),
                  .duringPlayback = false,
                  .capture = capture,
                  .envelope = envelopeOf(ring, static_cast<size_t>(kWatchRate) * kEnvelopeFrameMs / 1000),
                  .envelopeStart = capture->start,
                  .transcript = std::move(transcript)};
    capture->samples = std::move(ring);
    if (!capture->complete)
      captures_.push_back(capture);
  }
  out.duringPlayback = input_.playing && input_.playing(input_.cameraId);
  if (input_.onFire)
    input_.onFire(std::move(out));
}
