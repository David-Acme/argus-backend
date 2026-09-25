#include "vlm-rpc-server.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <opencv2/imgcodecs.hpp>
#include <response/response-rpc.hxx>
#include <vlm.grpc.pb.h>
#include <vlm/vlm-errors.hxx>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <semaphore>
#include <stdexcept>

namespace
{
namespace wire = argus::vlm::v1;
using Clock = std::chrono::system_clock;

constexpr std::size_t kMaxImageBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaxPromptBytes = 512;
constexpr std::size_t kMaxCameraIdBytes = 64;
constexpr int kMaxTokens = 4096;
constexpr std::chrono::seconds kTimeoutHeaderRounding{1};
constexpr auto kMaxDeadline = argus::vlm::kMaxTimeout + kTimeoutHeaderRounding;
constexpr int kMaxReceiveBytes = 64 * 1024 * 1024;

ResponseException stoppedError(const grpc::ServerContext& context)
{
  if (Clock::now() >= context.deadline())
    return {504, VlmErrors::DeadlineExceeded};
  return {499, VlmErrors::Cancelled};
}

bool authorized(grpc::ServerContext& context,
                const std::vector<std::pair<std::string, std::string>>& credentials)
{
  const auto& metadata = context.client_metadata();
  const auto range = metadata.equal_range(argus::client::kCallerCredentialKey);
  if (range.first == range.second || std::next(range.first) != range.second)
    return false;
  const std::string presented(range.first->second.data(), range.first->second.size());
  return std::ranges::any_of(credentials, [&presented](const auto& credential) {
    return !credential.first.empty() && !credential.second.empty() &&
           argus::client::constantTimeEquals(presented, credential.second);
  });
}

bool validRequest(const wire::DescribeRequest& request)
{
  return !request.image_jpeg().empty() &&
         request.image_jpeg().size() <= kMaxImageBytes &&
         request.prompt().size() <= kMaxPromptBytes &&
         request.camera_id().size() <= kMaxCameraIdBytes &&
         request.max_tokens() >= 0 && request.max_tokens() <= kMaxTokens;
}

cv::Mat decodeJpeg(const std::string& jpeg)
{
  const cv::Mat raw(1, static_cast<int>(jpeg.size()), CV_8UC1,
                    const_cast<char*>(jpeg.data()));
  return cv::imdecode(raw, cv::IMREAD_COLOR);
}
}

struct VlmRpcServer::Impl final : wire::Vision::Service
{
  explicit Impl(VlmRpcInput input)
      : input_(std::move(input)), slots_(std::max(1, input_.slots))
  {
    if (!input_.describe || !input_.capabilities || input_.credentials.empty())
      throw std::invalid_argument("Invalid VLM RPC configuration");
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(kMaxReceiveBytes);
    builder.AddListeningPort(input_.address, grpc::InsecureServerCredentials(),
                             &port_);
    builder.RegisterService(this);
    server_ = builder.BuildAndStart();
    if (!server_)
      throw std::runtime_error("VLM RPC listener failed");
  }

  grpc::Status Capabilities(grpc::ServerContext* context,
                            const wire::CapabilitiesRequest*,
                            wire::CapabilitiesResponse* response) override
  {
    if (!authorized(*context, input_.credentials))
      return argus::response::toRpcStatus(
          ResponseException(401, VlmErrors::Unauthorized));
    argus::vlm::Capabilities capabilities;
    try {
      capabilities = input_.capabilities();
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(
          ResponseException(500, VlmErrors::InternalError));
    }
    response->set_loaded(capabilities.loaded);
    response->set_max_input_px(capabilities.maxInputPx);
    response->set_default_max_tokens(capabilities.defaultMaxTokens);
    return grpc::Status::OK;
  }

  grpc::Status Describe(grpc::ServerContext* context,
                        const wire::DescribeRequest* request,
                        wire::DescribeResponse* response) override
  {
    if (!authorized(*context, input_.credentials))
      return argus::response::toRpcStatus(
          ResponseException(401, VlmErrors::Unauthorized));
    if (!validRequest(*request))
      return argus::response::toRpcStatus(
          ResponseException(400, VlmErrors::InvalidRequest));
    if (context->deadline() != Clock::time_point::max() &&
        context->deadline() > Clock::now() + kMaxDeadline)
      return argus::response::toRpcStatus(
          ResponseException(400, VlmErrors::InvalidRequest));
    if (context->IsCancelled() || Clock::now() >= context->deadline())
      return argus::response::toRpcStatus(stoppedError(*context));
    if (!slots_.try_acquire())
      return argus::response::toRpcStatus(
          ResponseException(429, VlmErrors::Busy));
    struct Release
    {
      std::counting_semaphore<>& slots;
      ~Release() { slots.release(); }
    } release{slots_};
    try {
      const cv::Mat bgr = decodeJpeg(request->image_jpeg());
      if (bgr.empty())
        return argus::response::toRpcStatus(
            ResponseException(400, VlmErrors::ImageNotDecodable));
      response->set_caption(input_.describe(VisionDescribeMatInput{
          .bgr = bgr,
          .prompt = request->prompt(),
          .maxTokens = request->max_tokens()}));
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(
          ResponseException(500, VlmErrors::InternalError));
    }
    return grpc::Status::OK;
  }

  VlmRpcInput input_;
  std::counting_semaphore<> slots_;
  int port_{0};
  std::unique_ptr<grpc::Server> server_;
};

VlmRpcServer::VlmRpcServer(VlmRpcInput input)
    : impl_(std::make_unique<Impl>(std::move(input)))
{
}

VlmRpcServer::~VlmRpcServer() { shutdown(); }
int VlmRpcServer::port() const { return impl_->port_; }
void VlmRpcServer::shutdown()
{
  if (impl_->server_) {
    impl_->server_->Shutdown(Clock::now());
    impl_->server_->Wait();
    impl_->server_.reset();
  }
}
