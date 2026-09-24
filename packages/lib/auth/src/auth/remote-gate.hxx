#pragma once

#include <auth/remote-config.hxx>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

class RemoteGate
{
public:
  explicit RemoteGate(RemoteConfig config);

  drogon::HttpResponsePtr check(const drogon::HttpRequestPtr& req,
                                bool remote);

private:
  RemoteConfig config_;
};
