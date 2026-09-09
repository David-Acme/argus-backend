#include "grpc-client-base.hxx"

#include <chrono>

namespace argus::sdk
{

namespace
{
constexpr int kKeepaliveTimeMs = 30000;
constexpr int kKeepaliveTimeoutMs = 10000;
constexpr int kMaxPingsWithoutData = 0;
} // namespace

std::shared_ptr<grpc::Channel> makeChannel(const std::string& target)
{
  return grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
}

std::shared_ptr<grpc::Channel> makeStreamingChannel(const std::string& target)
{
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, kKeepaliveTimeMs);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, kKeepaliveTimeoutMs);
  args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, kMaxPingsWithoutData);
  return grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(),
                                   args);
}

void setDeadline(grpc::ClientContext& context, int timeoutMs)
{
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(timeoutMs));
}

void addCallerIdentity(grpc::ClientContext& context,
                       const CallerIdentity& identity)
{
  context.AddMetadata("x-argus-user", std::to_string(identity.userId));
  context.AddMetadata("x-argus-role", identity.role);
  if (identity.device)
    context.AddMetadata("x-argus-device", *identity.device);
}

void addFleetSecret(grpc::ClientContext& context, const std::string& secret)
{
  if (!secret.empty())
    context.AddMetadata(kFleetSecretKey, secret);
}

} // namespace argus::sdk
