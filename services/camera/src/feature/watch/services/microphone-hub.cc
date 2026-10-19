#include "microphone-hub.hxx"

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace
{

namespace v1 = argus::camera::v1;
using Clock = std::chrono::steady_clock;

constexpr int64_t kSampleRate = 16000;

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}

class MicrophoneHub::Reader
{
public:
  explicit Reader(int64_t cameraId) : cameraId_(cameraId), emptySince_(Clock::now()) {}

  [[nodiscard]] int64_t cameraId() const { return cameraId_; }

  bool add(uint64_t id, const MicrophoneSubscriber& subscriber)
  {
    std::scoped_lock lock(mutex_);
    if (closing_)
      return false;
    subscribers_.emplace(id, subscriber);
    return true;
  }

  void remove(uint64_t id)
  {
    std::scoped_lock lock(mutex_);
    if (subscribers_.erase(id) > 0 && subscribers_.empty())
      emptySince_ = Clock::now();
  }

  [[nodiscard]] bool closing() const
  {
    std::scoped_lock lock(mutex_);
    return closing_;
  }

  [[nodiscard]] bool idleFor(std::chrono::milliseconds linger)
  {
    std::scoped_lock lock(mutex_);
    if (closing_)
      return true;
    if (!subscribers_.empty() || Clock::now() - emptySince_ < linger)
      return false;
    closing_ = true;
    return true;
  }

  void deliver(const MicrophoneChunk& chunk)
  {
    std::scoped_lock lock(mutex_);
    ids_.clear();
    for (const auto& [id, subscriber] : subscribers_)
      ids_.push_back(id);
    for (const uint64_t id : ids_) {
      const auto found = subscribers_.find(id);
      if (found != subscribers_.end() && found->second.onFrame)
        found->second.onFrame(chunk);
    }
  }

  void closeAll(v1::WatchRefusal refusal)
  {
    std::scoped_lock lock(mutex_);
    closing_ = true;
    auto closed = std::exchange(subscribers_, {});
    for (const auto& [id, subscriber] : closed)
      if (subscriber.onClosed)
        subscriber.onClosed(refusal);
  }

  bool sleep(const std::stop_token& stop, std::chrono::milliseconds duration)
  {
    std::unique_lock lock(waitMutex_);
    static_cast<void>(wake_.wait_for(lock, stop, duration, [] { return false; }));
    return !stop.stop_requested();
  }

  std::jthread thread;

private:
  int64_t cameraId_{0};
  mutable std::recursive_mutex mutex_;
  std::unordered_map<uint64_t, MicrophoneSubscriber> subscribers_;
  std::vector<uint64_t> ids_;
  Clock::time_point emptySince_;
  bool closing_{false};
  std::mutex waitMutex_;
  std::condition_variable_any wake_;
};

MicrophoneHub::MicrophoneHub(MicrophoneHubInput input) : input_(std::move(input)) {}

MicrophoneHub::~MicrophoneHub()
{
  shutdown();
}

size_t MicrophoneHub::frameSamples(int64_t frameMs)
{
  return static_cast<size_t>(std::max<int64_t>(1, kSampleRate * frameMs / 1000));
}

MicrophoneSubscribed MicrophoneHub::subscribe(const MicrophoneSubscriber& subscriber)
{
  if (subscriber.cameraId <= 0 || !subscriber.onFrame)
    return {.id = 0, .refusal = v1::WATCH_REFUSAL_UNKNOWN_CAMERA};
  if (const v1::WatchRefusal refusal = input_.gate ? input_.gate(subscriber.cameraId)
                                                   : v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
      refusal != v1::WATCH_REFUSAL_NONE)
    return {.id = 0, .refusal = refusal};

  constexpr int kAttempts = 3;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    std::shared_ptr<Reader> reader;
    uint64_t id = 0;
    bool joined = false;
    std::vector<std::shared_ptr<Reader>> retired;
    {
      std::scoped_lock lock(mutex_);
      if (stopping_)
        return {.id = 0, .refusal = v1::WATCH_REFUSAL_SHUTTING_DOWN};
      auto& slot = readers_[subscriber.cameraId];
      if (slot && slot->closing()) {
        retired_.push_back(slot);
        slot.reset();
      }
      id = ++nextId_;
      if (!slot) {
        const auto live = std::ranges::count_if(readers_, [](const auto& entry) {
          return entry.second != nullptr && !entry.second->closing();
        });
        if (std::cmp_greater_equal(live, input_.config.maxListeners)) {
          readers_.erase(subscriber.cameraId);
          return {.id = 0, .refusal = v1::WATCH_REFUSAL_CAPACITY};
        }
        slot = std::make_shared<Reader>(subscriber.cameraId);
        joined = slot->add(id, subscriber);
        const std::shared_ptr<Reader> fresh = slot;
        slot->thread = std::jthread([this, fresh](const std::stop_token& stop) { run(fresh, stop); });
      }
      reader = slot;
      subscriptions_[id] = subscriber.cameraId;
      retired.swap(retired_);
    }
    if (!joined)
      joined = reader->add(id, subscriber);
    reap(retired);
    if (joined)
      return {.id = id, .refusal = v1::WATCH_REFUSAL_NONE};
    std::scoped_lock lock(mutex_);
    subscriptions_.erase(id);
  }
  return {.id = 0, .refusal = v1::WATCH_REFUSAL_CAPACITY};
}

void MicrophoneHub::unsubscribe(uint64_t id)
{
  std::shared_ptr<Reader> reader;
  {
    std::scoped_lock lock(mutex_);
    const auto found = subscriptions_.find(id);
    if (found == subscriptions_.end())
      return;
    const int64_t cameraId = found->second;
    subscriptions_.erase(found);
    if (const auto live = readers_.find(cameraId); live != readers_.end())
      reader = live->second;
  }
  if (reader)
    reader->remove(id);
}

void MicrophoneHub::reap(std::vector<std::shared_ptr<Reader>>& retired)
{
  for (const auto& reader : retired) {
    reader->thread.request_stop();
    if (reader->thread.joinable() && reader->thread.get_id() != std::this_thread::get_id())
      reader->thread.join();
  }
  retired.clear();
}

void MicrophoneHub::shutdown()
{
  std::vector<std::shared_ptr<Reader>> all;
  {
    std::scoped_lock lock(mutex_);
    if (stopping_ && readers_.empty() && retired_.empty())
      return;
    stopping_ = true;
    for (auto& [cameraId, reader] : readers_)
      if (reader)
        all.push_back(reader);
    readers_.clear();
    subscriptions_.clear();
    all.insert(all.end(), retired_.begin(), retired_.end());
    retired_.clear();
  }
  for (const auto& reader : all)
    reader->thread.request_stop();
  for (const auto& reader : all)
    if (reader->thread.joinable() && reader->thread.get_id() != std::this_thread::get_id())
      reader->thread.join();
  for (const auto& reader : all)
    reader->closeAll(v1::WATCH_REFUSAL_SHUTTING_DOWN);
}

size_t MicrophoneHub::readers() const
{
  std::scoped_lock lock(mutex_);
  return static_cast<size_t>(std::ranges::count_if(readers_, [](const auto& entry) {
    return entry.second != nullptr && !entry.second->closing();
  }));
}

size_t MicrophoneHub::subscribers() const
{
  std::scoped_lock lock(mutex_);
  return subscriptions_.size();
}

void MicrophoneHub::run(const std::shared_ptr<Reader>& reader, const std::stop_token& stop)
{
  const int64_t cameraId = reader->cameraId();
  const CameraWatchConfig& config = input_.config;
  const size_t frame = frameSamples(config.micFrameMs);
  const auto linger = std::chrono::milliseconds(config.micLingerMs);
  const auto readTimeout = std::chrono::milliseconds(config.micFrameMs * 4);
  const auto recheck = std::chrono::milliseconds(config.gateRecheckMs);
  auto backoff = std::chrono::milliseconds(config.micReconnectMs);
  const auto maxBackoff = std::chrono::milliseconds(config.micReconnectMaxMs);
  uint64_t sequence = 0;
  bool gap = false;
  int failures = 0;
  std::vector<int16_t> pending;
  pending.reserve(frame * 4);

  const auto refused = [&]() {
    const v1::WatchRefusal refusal = input_.gate ? input_.gate(cameraId) : v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
    if (refusal == v1::WATCH_REFUSAL_NONE)
      return false;
    LOG_INFO << "Camera watch: camera " << cameraId << " stops listening ("
             << v1::WatchRefusal_Name(refusal) << ")";
    reader->closeAll(refusal);
    return true;
  };

  while (!stop.stop_requested()) {
    if (reader->idleFor(linger) || refused())
      return;
    std::unique_ptr<MicrophoneSource> source = input_.sources ? input_.sources(cameraId) : nullptr;
    if (!source || !source->open()) {
      if (++failures == 1 || failures % 30 == 0)
        LOG_WARN << "Camera watch: camera " << cameraId << " microphone did not open (attempt "
                 << failures << ")";
      gap = true;
      if (!reader->sleep(stop, backoff))
        return;
      backoff = std::min(backoff * 2, maxBackoff);
      continue;
    }
    pending.clear();
    auto nextCheck = Clock::now() + recheck;
    bool ended = false;
    while (!stop.stop_requested()) {
      const MicrophoneRead read = source->read(readTimeout);
      if (read.status == MicrophoneReadStatus::Ended) {
        ended = true;
        break;
      }
      if (read.status == MicrophoneReadStatus::Samples) {
        failures = 0;
        backoff = std::chrono::milliseconds(config.micReconnectMs);
        pending.insert(pending.end(), read.samples.begin(), read.samples.end());
        size_t offset = 0;
        while (pending.size() - offset >= frame) {
          reader->deliver({.pcm = std::span<const int16_t>(pending).subspan(offset, frame),
                           .sequence = ++sequence,
                           .capturedAtMs = nowMs(),
                           .gap = std::exchange(gap, false)});
          offset += frame;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
      }
      if (const auto now = Clock::now(); now >= nextCheck) {
        nextCheck = now + recheck;
        if (reader->idleFor(linger) || refused()) {
          source->close();
          return;
        }
      }
    }
    source->close();
    if (stop.stop_requested())
      return;
    if (ended) {
      gap = true;
      LOG_INFO << "Camera watch: camera " << cameraId << " microphone ended; reconnecting in "
               << backoff.count() << " ms";
      if (!reader->sleep(stop, backoff))
        return;
      backoff = std::min(backoff * 2, maxBackoff);
    }
  }
}
