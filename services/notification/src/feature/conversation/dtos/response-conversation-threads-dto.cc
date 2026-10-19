#include "response-conversation-threads-dto.hxx"

Json::Value ResponseConversationThreadsDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const auto& thread : threads) {
    Json::Value entry(Json::objectValue);
    entry["counterpartId"] = static_cast<Json::Int64>(thread.last.counterpartId);
    entry["counterpartName"] = thread.last.counterpartName;
    entry["unread"] = static_cast<Json::Int64>(thread.unread);
    entry["last"] = thread.last.toJson();
    list.append(std::move(entry));
  }
  return list;
}
