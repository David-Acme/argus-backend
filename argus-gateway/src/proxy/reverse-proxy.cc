#include "reverse-proxy.hxx"

#include <shared/wrapper/api-response/api-response.hxx>

using namespace drogon;
using namespace gateway_proxy;

void SimpleReverseProxy::initAndStart(const Json::Value &config)
{
    if (config.isMember("backends") && config["backends"].isArray())
    {
        for (auto &backend : config["backends"])
        {
            backendAddrs_.emplace_back(backend.asString());
        }
        if (backendAddrs_.empty())
        {
            LOG_ERROR << "You must set at least one backend";
            abort();
        }
    }
    else
    {
        LOG_ERROR << "Error in configuration";
        abort();
    }
    if (config.isMember("exclusions") && config["exclusions"].isArray())
    {
        for (auto &exclusion : config["exclusions"])
        {
            exclusions_.emplace_back(exclusion.asString());
        }
    }
    pipeliningDepth_ = config.get("pipelining", 0).asInt();
    sameClientToSameBackend_ =
        config.get("same_client_to_same_backend", false).asBool();
    connectionFactor_ = config.get("connection_factor", 1).asInt();
    if (connectionFactor_ == 0 || connectionFactor_ > 100)
    {
        LOG_ERROR << "invalid number of connection factor";
        abort();
    }
    if (config.isMember("routes") && config["routes"].isArray())
    {
        for (auto &route : config["routes"])
        {
            RouteTarget target;
            if (route.isMember("prefixes") && route["prefixes"].isArray())
            {
                for (auto &prefix : route["prefixes"])
                    target.prefixes.emplace_back(prefix.asString());
            }
            target.maxSegments = route.get("max_segments", 0).asInt();
            target.backend = route.get("backend", "").asString();
            if (target.prefixes.empty() || target.maxSegments == 0 ||
                target.backend.empty())
            {
                LOG_ERROR << "Route table entries need prefixes,"
                          << " max_segments and backend";
                abort();
            }
            routes_.emplace_back(std::move(target));
        }
    }
    clients_.init(
        [this](std::vector<HttpClientPtr> &clients, size_t) {
            clients.resize(backendAddrs_.size() * connectionFactor_);
        });
    routeClients_.init(
        [this](std::vector<HttpClientPtr> &clients, size_t) {
            clients.resize(routes_.size() * connectionFactor_);
        });
    clientIndex_.init(
        [this](size_t &index, size_t ioLoopIndex) {
            index = ioLoopIndex;
        });
    drogon::app().registerPreRoutingAdvice([this](const HttpRequestPtr &req,
                                                  AdviceCallback &&callback,
                                                  AdviceChainCallback &&pass) {
        preRouting(req, std::move(callback), std::move(pass));
    });
}

void SimpleReverseProxy::shutdown()
{
}

bool SimpleReverseProxy::excluded(const drogon::HttpRequestPtr &req) const
{
    for (const auto &prefix : exclusions_)
    {
        if (req->path() == prefix)
            return true;
        if (req->path().rfind(prefix + "/", 0) == 0)
            return true;
    }
    return false;
}

bool SimpleReverseProxy::segmentPrefixMatch(const std::string &path,
                                            const std::string &prefix)
{
    if (path == prefix)
        return true;
    if (path.rfind(prefix + "/", 0) == 0)
        return true;
    return false;
}

size_t SimpleReverseProxy::segmentCount(const std::string &path)
{
    size_t count = 0;
    size_t begin = 0;
    while (begin < path.size())
    {
        const size_t end = path.find('/', begin);
        const size_t len = end == std::string::npos ? std::string::npos
                                                    : end - begin;
        const std::string segment = path.substr(begin, len);
        if (!segment.empty())
            ++count;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return count;
}

int SimpleReverseProxy::matchRoute(const std::string &path) const
{
    for (size_t i = 0; i < routes_.size(); ++i)
    {
        const auto &route = routes_[i];
        for (const auto &prefix : route.prefixes)
        {
            if (!segmentPrefixMatch(path, prefix))
                continue;
            // The segment cap keeps the deeper control paths (/camera/{id}/
            // ptz, preset, settings, status, presets, capabilities, talk) on
            // the legacy backend while the two-segment CRUD goes to
            // argus-camera.
            if (segmentCount(path) <= route.maxSegments)
                return static_cast<int>(i);
        }
    }
    return -1;
}

void SimpleReverseProxy::preRouting(const HttpRequestPtr &req,
                                    AdviceCallback &&callback,
                                    AdviceChainCallback &&pass)
{
    if (excluded(req))
    {
        pass();
        return;
    }

    const int routeIndex = matchRoute(req->path());
    auto &clientsVector =
        routeIndex < 0 ? *clients_ : *routeClients_;
    if (routeIndex < 0)
    {
        size_t index;
        if (sameClientToSameBackend_)
        {
            index = std::hash<uint32_t>{}(req->getPeerAddr().ipNetEndian()) %
                    clientsVector.size();
            index = (index + (++(*clientIndex_)) * backendAddrs_.size()) %
                    clientsVector.size();
        }
        else
        {
            index = ++(*clientIndex_) % clientsVector.size();
        }
        auto &clientPtr = clientsVector[index];
        if (!clientPtr)
        {
            auto &addr = backendAddrs_[index % backendAddrs_.size()];
            clientPtr = HttpClient::newHttpClient(
                addr, trantor::EventLoop::getEventLoopOfCurrentThread());
            clientPtr->setPipeliningDepth(pipeliningDepth_);
        }
        forward(req, std::move(callback), clientPtr);
        return;
    }

    const size_t base = static_cast<size_t>(routeIndex) * connectionFactor_;
    const size_t index = base + (++(*clientIndex_)) % connectionFactor_;
    auto &clientPtr = clientsVector[index];
    if (!clientPtr)
    {
        clientPtr = HttpClient::newHttpClient(
            routes_[routeIndex].backend,
            trantor::EventLoop::getEventLoopOfCurrentThread());
        clientPtr->setPipeliningDepth(pipeliningDepth_);
    }
    forward(req, std::move(callback), clientPtr);
}

void SimpleReverseProxy::forward(const HttpRequestPtr &req,
                                 AdviceCallback &&callback,
                                 HttpClientPtr &clientPtr)
{
    req->setPassThrough(true);
    // The gateway is the only one that sees the client: replace any
    // client-supplied value with the observed TCP peer address, exactly like
    // the /sync relay does (the legacy device hash is HMAC(User-Agent|IP)).
    req->removeHeader("x-forwarded-for");
    req->addHeader("X-Forwarded-For", req->getPeerAddr().toIp());
    clientPtr->sendRequest(
        req,
        [callback = std::move(callback)](ReqResult result,
                                         const HttpResponsePtr &resp) {
            if (result == ReqResult::Ok)
            {
                resp->setPassThrough(true);
                callback(resp);
            }
            else
            {
                // Deviation from the vendored example: a bare 500 would break
                // the {status, info, errors} wire contract the app parses.
                callback(ApiResponse::error(
                    500, "INTERNAL_ERROR", "Legacy backend is unreachable"));
            }
        });
}
