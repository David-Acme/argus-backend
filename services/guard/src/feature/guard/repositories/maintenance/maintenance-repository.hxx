#pragma once

#include "maintenance-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

class MaintenanceRepository
{
public:
  [[nodiscard]] drogon::Task<MaintenanceWindow> create(const MaintenanceCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<MaintenanceWindow>> findById(int64_t id) const;

  [[nodiscard]] drogon::Task<std::vector<MaintenanceWindow>> open() const;

  [[nodiscard]] drogon::Task<std::vector<MaintenanceWindow>> recent(int64_t since) const;

  [[nodiscard]] drogon::Task<bool> end(const MaintenanceEndInput& input) const;

  [[nodiscard]] drogon::Task<void> setViewChecks(const MaintenanceViewChecksInput& input) const;

  [[nodiscard]] drogon::Task<bool> recentDuress(const DuressLookupInput& input) const;

  [[nodiscard]] drogon::Task<TamperAsk> createAsk(const TamperAskCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<TamperAsk>> findAsk(int64_t id) const;

  [[nodiscard]] drogon::Task<std::optional<TamperAsk>> askForOnset(const TamperAskOnsetInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<TamperAsk>> pendingAsks() const;

  [[nodiscard]] drogon::Task<bool> answerAsk(const TamperAskAnswerInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<IncidentRef>> incident(int64_t id) const;
};
