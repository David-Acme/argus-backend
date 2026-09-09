#pragma once

#include <drogon/plugins/Plugin.h>
#include <drogon/drogon.h>
#include <string>
#include <vector>

// Vendored Drogon SimpleReverseProxy with exclusions, XFF and a route table.
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
    // Index into routes_, or -1 when the request falls through.
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
