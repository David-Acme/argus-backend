#include "cors.hxx"

void Cors::apply(const drogon::HttpResponsePtr& resp)
{
  resp->addHeader("Access-Control-Allow-Methods",
                  "GET, POST, PATCH, PUT, DELETE, OPTIONS");
  resp->addHeader("Access-Control-Allow-Headers",
                  "Content-Type, Authorization, Accept");
  resp->addHeader("Access-Control-Max-Age", "86400");
}

void Cors::handleOptions(
    const drogon::HttpRequestPtr&,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  apply(resp);
  callback(resp);
}
