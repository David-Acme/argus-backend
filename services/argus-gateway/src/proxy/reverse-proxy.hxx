#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/drogon.h>
#include <string>
#include <vector>

// Official Drogon SimpleReverseProxy pattern (examples/simple_reverse_proxy),
// vendored with three additive behaviors required by the cutover:
//   - path exclusions: gateway-native routes pass through to the normal
//     routing chain instead of being forwarded;
//   - X-Forwarded-For synthesized from the observed TCP peer address (same
//     rule the /sync relay applies), so the DeviceFilter device-hash binds
//     the real client and a client-supplied header is never trusted;
//   - route table: prefix + max-segment routes forward to their own backend;
//     a request no route claims falls through to the normal routing chain,
//     which answers unknown paths with the frozen NOT_FOUND envelope.
namespace gateway_proxy
{
class SimpleReverseProxy : public drogon::Plugin<SimpleReverseProxy>
{
  public:
    SimpleReverseProxy() = default;

    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    struct RouteTarget
    {
        std::vector<std::string> prefixes;
        size_t maxSegments{0};
        std::string backend;
    };

    // Route-table resolution, exposed for the gateway test suite.
    static bool segmentPrefixMatch(const std::string &path,
                                   const std::string &prefix);
    static size_t segmentCount(const std::string &path);
    // Index into routes_, or -1 when the request falls through to the
    // normal routing chain.
    int matchRoute(const std::string &path) const;

  private:
    void preRouting(const drogon::HttpRequestPtr &,
                    drogon::AdviceCallback &&,
                    drogon::AdviceChainCallback &&);
    void forward(const drogon::HttpRequestPtr &,
                 drogon::AdviceCallback &&,
                 drogon::HttpClientPtr &client);
    bool excluded(const drogon::HttpRequestPtr &req) const;

    drogon::IOThreadStorage<std::vector<drogon::HttpClientPtr>> routeClients_;
    drogon::IOThreadStorage<size_t> clientIndex_{0};
    std::vector<std::string> exclusions_;
    std::vector<RouteTarget> routes_;
    size_t pipeliningDepth_{0};
    size_t connectionFactor_{1};
};
}  // namespace gateway_proxy
