#include "wind-down-repository.hxx"

#include <string>

using namespace wind_down_query;

drogon::Task<WindDownReport> WindDownRepository::run(const WindDownInput& input) const
{
  WindDownReport report;
  report.observations = static_cast<int64_t>(
      (co_await input.client->execSqlCoro(std::string(DROP_OBSERVATIONS), input.at, input.at)).affectedRows());
  report.actions = static_cast<int64_t>(
      (co_await input.client->execSqlCoro(std::string(DROP_ACTIONS), std::string(kDropReason), input.at))
          .affectedRows());
  report.duty =
      static_cast<int64_t>((co_await input.client->execSqlCoro(std::string(END_DUTY), input.at)).affectedRows());
  co_return report;
}
