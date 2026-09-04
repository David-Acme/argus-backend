#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/drogon.h>
#include <string>
#include <vector>

// Official Drogon SimpleReverseProxy pattern (examples/simple_reverse_proxy),
// vendored with two additive behaviors required by the cutover:
//   - path exclusions: gateway-native routes pass through to the normal
//     routing chain instead of being forwarded (the legacy keeps its own
//     identity routes in the binary, so it must never see them here);
//   - X-Forwarded-For synthesized from the observed TCP peer address (same
//     rule the /sync relay applies), so the legacy device-hash filter binds
//     the real client and a client-supplied header is never trusted.
namespace gateway_proxy
{
class SimpleReverseProxy : public drogon::Plugin<SimpleReverseProxy>
{
  public:
    SimpleReverseProxy() = default;

    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

  private:
    void preRouting(const drogon::HttpRequestPtr &,
                    drogon::AdviceCallback &&,
                    drogon::AdviceChainCallback &&);
    bool excluded(const drogon::HttpRequestPtr &req) const;

    drogon::IOThreadStorage<std::vector<drogon::HttpClientPtr>> clients_;
    drogon::IOThreadStorage<size_t> clientIndex_{0};
    std::vector<std::string> backendAddrs_;
    std::vector<std::string> exclusions_;
    bool sameClientToSameBackend_{false};
    size_t pipeliningDepth_{0};
    size_t connectionFactor_{1};
};
}  // namespace gateway_proxy