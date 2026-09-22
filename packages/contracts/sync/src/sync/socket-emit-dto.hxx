#pragma once

#include <json/value.h>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>

// The triple every change-feed payload carries: the operation, the table it
// happened on, and the row itself. The three keys are frozen wire
// (docs/architecture/wire-nats-subjects.md); an in-flight change is `operation`
// as an int, `option` as the table name, `info` as the object.
struct SocketEmitDto
{
  SyncOperation operation{SyncOperation::Synchronize};
  TableName option{TableName::User};
  Json::Value obj;

  // Inline because a contract is an INTERFACE target: there is no translation
  // unit to carry this and nothing about it needs one.
  Json::Value toJson() const
  {
    Json::Value json;
    json["operation"] = static_cast<int>(operation);
    json["option"] = tableNameToString(option);
    json["info"] = obj;
    return json;
  }
};
