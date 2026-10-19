#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string_view>
#include <vector>

enum class WakeVerdict : uint8_t
{
  Accepted,
  Rejected,
  DuplicateWake,
  Broadcast,
  SelfEcho,
  CrossEcho,
  LowSnr,
  OnCall,
  BusyLine,
  Unconfirmed
};

[[nodiscard]] std::string_view wakeVerdictToString(WakeVerdict verdict);

struct WakeFireRecord
{
  int64_t cameraId{0};
  std::chrono::system_clock::time_point at{};
  WakeVerdict verdict{WakeVerdict::Unconfirmed};
  std::string_view source;
  float snrDb{0.0F};
  int distance{-1};
  float stageTwo{0.0F};
  float score{0.0F};
};

class FireLog
{
public:
  explicit FireLog(size_t capacity);

  void record(const WakeFireRecord& record);
  [[nodiscard]] std::vector<WakeFireRecord> recent() const;

private:
  size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<WakeFireRecord> records_;
};
