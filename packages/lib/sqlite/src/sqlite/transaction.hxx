#pragma once

#include <coroutine>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>

namespace db_transaction
{
class CommitObserver
{
public:
  explicit CommitObserver(std::function<void()> onCommitted);
  ~CommitObserver();
  CommitObserver(const CommitObserver&) = delete;
  CommitObserver& operator=(const CommitObserver&) = delete;

  static void notifyAll();

private:
  std::function<void()> onCommitted_;
};

drogon::Task<std::shared_ptr<drogon::orm::Transaction>>
begin(const drogon::orm::DbClientPtr& client);

class Commit
{
public:
  explicit Commit(std::shared_ptr<drogon::orm::Transaction> transaction)
      : transaction_(std::move(transaction))
  {
  }

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) noexcept
  {
    auto transaction = std::move(transaction_);
    if (!transaction) {
      committed_ = false;
      handle.resume();
      return;
    }
    transaction->setCommitCallback([this, handle](bool committed) {
      committed_ = committed;
      if (committed)
        CommitObserver::notifyAll();
      handle.resume();
    });
    transaction.reset();
  }

  bool await_resume() const noexcept { return committed_; }

private:
  std::shared_ptr<drogon::orm::Transaction> transaction_;
  bool committed_{false};
};

void rollback(const std::shared_ptr<drogon::orm::Transaction>& transaction);
}
