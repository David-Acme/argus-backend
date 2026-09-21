#pragma once

#include <errors/response-exception.hxx>
#include <grpcpp/support/status.h>

namespace argus::response
{
grpc::Status toRpcStatus(const ResponseException& error);
ResponseException fromRpcStatus(const grpc::Status& status);
}
