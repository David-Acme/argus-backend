#include "api-response.hxx"

drogon::HttpResponsePtr ApiResponse::ok(const Json::Value& data)
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  Json::Value body;
  body["success"] = true;
  body["data"] = data;
  resp->setBody(body.toStyledString());
  return resp;
}

drogon::HttpResponsePtr ApiResponse::created(const Json::Value& data)
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k201Created);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  Json::Value body;
  body["success"] = true;
  body["data"] = data;
  resp->setBody(body.toStyledString());
  return resp;
}

drogon::HttpResponsePtr ApiResponse::noContent()
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k204NoContent);
  return resp;
}

drogon::HttpResponsePtr ApiResponse::error(drogon::HttpStatusCode status,
                                           const std::string& message)
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  Json::Value body;
  body["success"] = false;
  body["message"] = message;
  resp->setBody(body.toStyledString());
  return resp;
}
