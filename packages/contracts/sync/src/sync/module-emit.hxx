#pragma once

#include <drogon/orm/DbClient.h>
#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>

struct ModuleEmitInput
{
  TableName table{TableName::Camera};
  SocketEmitDto body;
  drogon::orm::DbClient* client{nullptr};
};
