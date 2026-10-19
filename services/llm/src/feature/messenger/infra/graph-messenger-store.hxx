#pragma once

#include <feature/messenger/services/messenger-store.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>

class GraphMessengerStore final : public MessengerStore
{
public:
  explicit GraphMessengerStore(SqliteGraph& graph);

  void run(const std::function<void(sqlite3*)>& body) const override;

private:
  SqliteGraph& graph_;
};
