#include "guard-module-wind-down.hxx"

#include <ctime>
#include <memory>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

drogon::Task<WindDownReport> GuardModuleWindDown::run() const
{
  std::shared_ptr<drogon::orm::Transaction> transaction;
  WindDownReport report;
  try {
    transaction = co_await db_transaction::begin(DbService::client());
    report = co_await repository_.run({.at = static_cast<int64_t>(std::time(nullptr)), .client = transaction.get()});
    if (!co_await db_transaction::Commit(std::move(transaction))) {
      LOG_WARN << "Guard: the surveillance wind-down was not committed";
      co_return WindDownReport{};
    }
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  if (!report.empty())
    LOG_INFO << "Guard: surveillance is off; dropped " << report.observations << " pending observation(s), "
             << report.actions << " pending alert action(s) and ended " << report.duty << " duty shift(s)";
  co_return report;
}
