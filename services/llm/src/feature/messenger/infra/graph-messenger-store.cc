#include "graph-messenger-store.hxx"

#include <mutex>

GraphMessengerStore::GraphMessengerStore(SqliteGraph& graph)
    : graph_(graph)
{
}

void GraphMessengerStore::run(const std::function<void(sqlite3*)>& body) const
{
  const std::scoped_lock lock(graph_.mutex());
  body(graph_.handle());
}
