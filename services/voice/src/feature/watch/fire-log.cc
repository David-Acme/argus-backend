#include "fire-log.hxx"

#include <trantor/utils/Logger.h>

std::string_view wakeVerdictToString(WakeVerdict verdict)
{
  switch (verdict) {
    case WakeVerdict::Accepted:
      return "accepted";
    case WakeVerdict::Rejected:
      return "rejected";
    case WakeVerdict::DuplicateWake:
      return "duplicate_wake";
    case WakeVerdict::Broadcast:
      return "broadcast";
    case WakeVerdict::SelfEcho:
      return "self_echo";
    case WakeVerdict::CrossEcho:
      return "cross_echo";
    case WakeVerdict::LowSnr:
      return "low_snr";
    case WakeVerdict::OnCall:
      return "on_call";
    case WakeVerdict::BusyLine:
      return "busy_line";
    case WakeVerdict::Unconfirmed:
      return "unconfirmed";
  }
  return "unconfirmed";
}

FireLog::FireLog(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void FireLog::record(const WakeFireRecord& record)
{
  LOG_INFO << "Voice watch: wake on camera " << record.cameraId << " source=" << record.source
           << " verdict=" << wakeVerdictToString(record.verdict) << " snr_db=" << record.snrDb
           << " distance=" << record.distance << " stage2=" << record.stageTwo << " score=" << record.score;
  std::scoped_lock lock(mutex_);
  records_.push_back(record);
  while (records_.size() > capacity_)
    records_.pop_front();
}

std::vector<WakeFireRecord> FireLog::recent() const
{
  std::scoped_lock lock(mutex_);
  return {records_.begin(), records_.end()};
}
