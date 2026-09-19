#include "socket-emit-dto.hxx"

Json::Value SocketEmitDto::toJson() const
{
  Json::Value json;
  json["operation"] = static_cast<int>(operation);
  json["option"] = tableNameToString(option);
  json["info"] = obj;
  return json;
}
