#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <grpc/grpc-server-drain.hxx>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>

using argus::client::GrpcServerDrain;

TEST_CASE("the gRPC drain shuts a live server down once and reports drained")
{
  grpc::CallbackGenericService service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterCallbackGenericService(&service);
  auto server = builder.BuildAndStart();
  REQUIRE(server);
  CHECK(port > 0);

  GrpcServerDrain drain(std::move(server), std::chrono::milliseconds(200));
  CHECK_FALSE(drain.drained());
  drain.requestStop();
  drain.requestStop();
  drain.stop();
  CHECK(drain.drained());
  drain.stop();
  CHECK(drain.drained());
}

TEST_CASE("a drain without a server is drained as soon as it is asked to stop")
{
  GrpcServerDrain empty(nullptr, std::chrono::milliseconds(200));
  CHECK_FALSE(empty.drained());
  empty.requestStop();
  CHECK(empty.drained());
}
