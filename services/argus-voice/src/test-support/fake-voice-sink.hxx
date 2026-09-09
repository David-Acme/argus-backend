#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <feature/voice/voice-engine-seam.hxx>
#include <feature/voice/voice-session-service.hxx>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Thread-safe VoiceSessionSink collecting the typed server frames a session emits.
class FakeVoiceSink final : public VoiceSessionSink
{
public:
  bool connected() const override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
  }

  void sendServerFrame(argus::voice::v1::ServerFrame frame) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.push_back(std::move(frame));
  }

  void close()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
  }

  std::vector<argus::voice::v1::ServerFrame> snapshot() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
  }

  std::vector<argus::voice::v1::ServerFrame> of(bool ttsChunk) const
  {
    const auto frames = snapshot();
    std::vector<argus::voice::v1::ServerFrame> out;
    for (const auto& frame : frames)
      if (frame.has_tts_chunk() == ttsChunk)
        out.push_back(frame);
    return out;
  }

  bool hasType(const std::string& type) const
  {
    for (const auto& frame : snapshot()) {
      if (frame.has_stt() && type == "voice:stt")
        return true;
      if (frame.has_assistant() && type == "voice:assistant")
        return true;
      if (frame.has_event() && type == "voice:event")
        return true;
      if (frame.has_done() && type == "voice:done")
        return true;
    }
    return false;
  }

private:
  mutable std::mutex mutex_;
  bool open_{true};
  std::vector<argus::voice::v1::ServerFrame> frames_;
};

// Test seam identity: every UpdateUser write is recorded, nothing leaves.
struct FakeIdentity final : IVoiceIdentity
{
  std::mutex mutex;
  std::vector<VoiceNameWrite> writes;

  void updateUserName(const VoiceNameWrite& write) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    writes.push_back(write);
  }
};

template <typename Pred>
bool waitFor(Pred ready, int ms = 5000)
{
  for (int waited = 0; waited < ms; waited += 20) {
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return ready();
}
