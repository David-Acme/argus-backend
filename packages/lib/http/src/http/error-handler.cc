#include "error-handler.hxx"

#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <http/api-response.hxx>
#include <http/details/http-errors.hxx>

#include <json/value.h>
#include <json/json.h>
#include <string>
#include <trantor/utils/Logger.h>

void ErrorHandler::handleException(
    const std::exception& e, const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& respCallback)
{
  if (const auto* ve = dynamic_cast<const ValidationException*>(&e)) {
    Json::Value errors;
    for (const auto& [field, msgs] : ve->errors()) {
      Json::Value arr(Json::arrayValue);
      for (const auto& m : msgs)
        arr.append(m);
      errors[field] = arr;
    }
    respCallback(ApiResponse::validationError(errors));
    return;
  }

  if (const auto* re = dynamic_cast<const ResponseException*>(&e)) {
    respCallback(ApiResponse::error(*re));
    return;
  }

  if (dynamic_cast<const Json::LogicError*>(&e) != nullptr) {
    LOG_WARN << "Malformed field type on " << req->methodString() << " "
             << req->path() << ": " << e.what();
    Json::Value errors(Json::objectValue);
    errors["body"].append("a field has the wrong type");
    respCallback(ApiResponse::validationError(errors));
    return;
  }

  LOG_ERROR << "Unhandled exception on " << req->methodString() << " "
            << req->path() << ": " << e.what();
  respCallback(ApiResponse::error(HttpErrors::InternalError));
}

drogon::HttpResponsePtr ErrorHandler::unmatchedRoute(drogon::HttpStatusCode status)
{
  return ApiResponse::error(status == drogon::k405MethodNotAllowed
                                ? HttpErrors::MethodNotAllowed
                                : HttpErrors::PathNotFound);
}
