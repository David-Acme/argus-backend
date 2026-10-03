#include "transaction.hxx"

#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace db_transaction
{
namespace
{
std::mutex& observersMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::vector<CommitObserver*>& observers()
{
  static std::vector<CommitObserver*> list;
  return list;
}
}

CommitObserver::CommitObserver(std::function<void()> onCommitted)
    : onCommitted_(std::move(onCommitted))
{
  std::scoped_lock lock(observersMutex());
  observers().push_back(this);
}

CommitObserver::~CommitObserver()
{
  std::scoped_lock lock(observersMutex());
  std::erase(observers(), this);
}

void CommitObserver::notifyAll()
{
  std::scoped_lock lock(observersMutex());
  for (auto* observer : observers())
    observer->onCommitted_();
}

drogon::Task<std::shared_ptr<drogon::orm::Transaction>>
begin(const drogon::orm::DbClientPtr& client)
{
  if (!client)
    throw std::invalid_argument("no database client to open a transaction on");
  co_return co_await client->newTransactionCoro(
      drogon::orm::TransactionType::Immediate);
}

void rollback(const std::shared_ptr<drogon::orm::Transaction>& transaction)
{
  if (transaction)
    transaction->rollback();
}
}
