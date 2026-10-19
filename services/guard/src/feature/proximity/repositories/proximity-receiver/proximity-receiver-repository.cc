#include "proximity-receiver-repository.hxx"

#include <sqlite/db-service.hxx>

#include <string>

using namespace proximity_receiver_query;

drogon::Task<std::vector<ProximityReceiverRow>> ProximityReceiverRepository::find() const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(SELECT_RECEIVERS));
  std::vector<ProximityReceiverRow> receivers;
  receivers.reserve(rows.size());
  for (const auto& row : rows) {
    const auto kind = proximityReceiverKindFromString(row["kind"].as<std::string>());
    if (!kind)
      continue;
    receivers.push_back({.id = row["id"].as<int64_t>(),
                         .name = row["name"].as<std::string>(),
                         .room = row["room"].as<std::string>(),
                         .kind = *kind,
                         .host = row["host"].as<std::string>(),
                         .port = row["port"].as<int64_t>(),
                         .createdAt = row["created_at"].as<int64_t>()});
  }
  co_return receivers;
}

drogon::Task<int64_t> ProximityReceiverRepository::count() const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(COUNT_RECEIVERS));
  co_return rows.empty() ? 0 : rows.front()["total"].as<int64_t>();
}

drogon::Task<ProximityReceiverRow> ProximityReceiverRepository::create(const ProximityReceiverCreateInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(INSERT_RECEIVER), input.name, input.room, proximityReceiverKindToString(input.kind), input.host,
      input.port, input.now);
  co_return ProximityReceiverRow{.id = static_cast<int64_t>(result.insertId()),
                                 .name = input.name,
                                 .room = input.room,
                                 .kind = input.kind,
                                 .host = input.host,
                                 .port = input.port,
                                 .createdAt = input.now};
}

drogon::Task<bool> ProximityReceiverRepository::remove(int64_t id) const
{
  const auto result = co_await DbService::client()->execSqlCoro(std::string(DELETE_RECEIVER), id);
  co_return result.affectedRows() > 0;
}
