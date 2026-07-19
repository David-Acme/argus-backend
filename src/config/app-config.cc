#include "app-config.hxx"

#include <drogon/HttpViewData.h>

drogon::HttpResponsePtr AppConfig::get404Response()
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k404NotFound);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  Json::Value body;
  body["success"] = false;
  body["message"] = "Not found";
  resp->setBody(body.toStyledString());
  return resp;
}

void AppConfig::handleException(const std::exception& e,
                                const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& respCallback)
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k500InternalServerError);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  Json::Value body;
  body["success"] = false;
  body["message"] = e.what();
  resp->setBody(body.toStyledString());
  respCallback(resp);
}
