#pragma once

#include <feature/duress/repositories/duress-raise/duress-raise-repository.hxx>
#include <feature/duress/services/duress-outbox.hxx>

#include <functional>
#include <memory>
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
  static constexpr int kWriterBusyTimeoutMs = 5000;

  explicit SqliteDuressOutbox(SqliteDuressOutboxInput input);

  [[nodiscard]] std::optional<int64_t> add(const DuressRaiseCreateInput& input) override;

  [[nodiscard]] std::optional<std::vector<DuressRaiseRow>> pending() override;

  void remove(int64_t id) override;

  [[nodiscard]] int synchronous();

private:
  [[nodiscard]] sqlite3* writer();

  std::mutex* mutex_{nullptr};
  std::function<sqlite3*()> db_;
  std::mutex writeMutex_;
  std::unique_ptr<sqlite3, int (*)(sqlite3*)> writer_;
  DuressRaiseRepository repository_;
};
