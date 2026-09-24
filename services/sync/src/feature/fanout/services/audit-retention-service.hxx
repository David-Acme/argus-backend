#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/audit-log-service.hxx>
#include <feature/fanout/services/user-audit-log-service.hxx>

#include <cstdint>
#include <optional>

class AuditRetentionService
{
public:
  AuditRetentionService() = default;
  ~AuditRetentionService();

  void start(int retentionDays);
  void stop();

private:
  [[nodiscard]] drogon::Task<int64_t> sweep(int64_t cutoffMs) const;
  void runSweep();
  static void clearTimer(std::optional<uint64_t>& timer);

  AuditLogService auditLogService_;
  UserAuditLogService userAuditLogService_;
  std::optional<uint64_t> warmupTimer_;
  std::optional<uint64_t> dailyTimer_;
  int retentionDays_{0};
  bool started_{false};
  bool running_{false};
};
