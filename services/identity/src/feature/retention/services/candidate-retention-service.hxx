#pragma once

#include <config/identity-config.hxx>
#include <feature/retention/repositories/candidate-retention/candidate-retention-repository.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <trantor/net/EventLoop.h>

class CandidateRetentionService
{
public:
  explicit CandidateRetentionService(IdentityRetentionConfig config);
  ~CandidateRetentionService();

  CandidateRetentionService(const CandidateRetentionService&) = delete;
  CandidateRetentionService& operator=(const CandidateRetentionService&) = delete;

  void start();
  void stop();

  drogon::Task<size_t> sweep(int64_t now);

private:
  drogon::Task<size_t> retireBatch(int64_t cutoff);
  void launch();

  IdentityRetentionConfig config_;
  CandidateRetentionRepository repository_;
  std::optional<trantor::TimerId> firstTimer_;
  std::optional<trantor::TimerId> timer_;
  std::atomic<bool> running_{false};
};
