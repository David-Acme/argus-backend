#pragma once

#include <feature/retention/repositories/candidate-retention/candidate-retention-repository.hxx>
#include <shared/repositories/visitor-setting/visitor-setting-repository.hxx>
#include <shared/repositories/pending-object-delete/pending-object-delete-repository.hxx>
#include <shared/services/privacy/privacy-gate.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <trantor/net/EventLoop.h>

class CandidateRetentionService
{
public:
  CandidateRetentionService() = default;
  ~CandidateRetentionService();

  CandidateRetentionService(const CandidateRetentionService&) = delete;
  CandidateRetentionService& operator=(const CandidateRetentionService&) = delete;

  void start();
  void stop();

  drogon::Task<size_t> sweep(int64_t now);
  [[nodiscard]] drogon::Task<int64_t> cutoffAt(int64_t now) const;

private:
  drogon::Task<size_t> retireBatch(int64_t cutoff);
  void launch();

  CandidateRetentionRepository repository_;
  VisitorSettingRepository settingRepository_;
  PrivacyGate privacyGate_;
  PendingObjectDeleteRepository pendingRepository_;
  std::optional<trantor::TimerId> firstTimer_;
  std::optional<trantor::TimerId> timer_;
  std::atomic<bool> running_{false};
};
