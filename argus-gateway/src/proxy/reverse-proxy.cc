#include "reverse-proxy.hxx"

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
    clients_.init(
        [this](std::vector<HttpClientPtr> &clients, size_t) {
            clients.resize(backendAddrs_.size() * connectionFactor_);
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

void SimpleReverseProxy::preRouting(const HttpRequestPtr &req,
                                    AdviceCallback &&callback,
                                    AdviceChainCallback &&pass)
{
    if (excluded(req))
    {
        pass();
        return;
    }

    size_t index;
    auto &clientsVector = *clients_;
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
                auto errResp = HttpResponse::newHttpResponse();
                errResp->setStatusCode(k500InternalServerError);
                callback(errResp);
            }
        });
}