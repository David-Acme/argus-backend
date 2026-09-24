#pragma once

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <vector>

struct ProductivityRpcInput
{
  std::vector<grpc::Service*> services;
};

class ProductivityRpcServer
{
public:
  explicit ProductivityRpcServer(const ProductivityRpcInput& input);
  ~ProductivityRpcServer();
  [[nodiscard]] bool listening() const;
  [[nodiscard]] const std::string& address() const;
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
