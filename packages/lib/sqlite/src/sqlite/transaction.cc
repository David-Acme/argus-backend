#include "transaction.hxx"

#include <stdexcept>

namespace db_transaction
{
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
