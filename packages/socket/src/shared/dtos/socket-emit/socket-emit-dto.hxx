#pragma once

#include <json/value.h>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>

struct SocketEmitDto
{
  SyncOperation operation{SyncOperation::Synchronize};
  TableName option{TableName::User};
  Json::Value obj;

  Json::Value toJson() const;
};
