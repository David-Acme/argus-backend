#pragma once

#include <drogon/HttpClient.h>

#include <string>

namespace go2rtc_http
{
drogon::HttpClientPtr client(const std::string& lane);
}
