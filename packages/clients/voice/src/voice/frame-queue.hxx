#pragma once

#include <argus/voice/v1/voice.pb.h>

#include <cstddef>
#include <deque>
#include <iterator>
#include <utility>

inline constexpr size_t kMaxPendingWrites = 1024;

class FrameQueue
{
public:
  bool push(argus::voice::v1::ClientFrame frame)
  {
    if (cancelled_ || pending_.size() >= kMaxPendingWrites)
      return false;
    pending_.push_back(std::move(frame));
    return true;
  }

  argus::voice::v1::ClientFrame* nextWrite()
  {
    if (writing_ || pending_.empty())
      return nullptr;
    writing_ = true;
    return &pending_.front();
  }

  void completeWrite(bool ok)
  {
    if (!ok)
      pending_.clear();
    else if (!pending_.empty())
      pending_.pop_front();
    writing_ = false;
  }

  void cancel()
  {
    cancelled_ = true;
    if (writing_ && !pending_.empty())
      pending_.erase(std::next(pending_.begin()), pending_.end());
    else
      pending_.clear();
  }

  [[nodiscard]] bool cancelled() const { return cancelled_; }

  [[nodiscard]] size_t size() const { return pending_.size(); }

private:
  std::deque<argus::voice::v1::ClientFrame> pending_;
  bool writing_{false};
  bool cancelled_{false};
};
