#pragma once

#include <drogon/orm/DbClient.h>
#include <feature/modules/repositories/module-journal/module-journal-query.hxx>

#include <cstdint>
#include <optional>

class ModuleJournalRepository
{
public:
  explicit ModuleJournalRepository(drogon::orm::DbClientPtr client);

  [[nodiscard]] std::optional<std::int64_t> cursor() const;
  void start(std::int64_t publishedThrough) const;
  void advance(const ModuleJournalAdvanceInput& input) const;

private:
  drogon::orm::DbClientPtr client_;
};
