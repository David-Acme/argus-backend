#pragma once

#include <json/value.h>
#include <shared/contracts/sync-operation.hxx>
#include <shared/enums.hxx>

struct SocketEmitDto
{
  SyncOperation operation{SyncOperation::Synchronize};
  TableName option{TableName::User};
  Json::Value obj;

  Json::Value toJson() const;
};
