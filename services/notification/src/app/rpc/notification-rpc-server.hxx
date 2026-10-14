#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <optional>
#include <string>
#include <vector>

struct NotificationRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

[[nodiscard]] argus::client::Listener
startNotificationRpc(const NotificationRpcListenInput& input);

[[nodiscard]] std::optional<std::string>
notificationRpcFailure(const NotificationRpcListenInput& input,
                       const argus::client::Listener& listener);
