#pragma once

#include <feature/duress/repositories/duress-raise/duress-raise-repository.hxx>
#include <feature/duress/services/duress-outbox.hxx>

#include <functional>
#include <mutex>

struct sqlite3;

struct SqliteDuressOutboxInput
{
  std::mutex* mutex{nullptr};
  std::function<sqlite3*()> db;
};

class SqliteDuressOutbox final : public DuressOutbox
{
public:
  explicit SqliteDuressOutbox(SqliteDuressOutboxInput input);

  [[nodiscard]] std::optional<int64_t> add(const DuressRaiseCreateInput& input) override;

  [[nodiscard]] std::optional<std::vector<DuressRaiseRow>> pending() override;

  void remove(int64_t id) override;

private:
  std::mutex* mutex_{nullptr};
  std::function<sqlite3*()> db_;
  DuressRaiseRepository repository_;
};
