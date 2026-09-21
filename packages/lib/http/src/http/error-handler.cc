#include "error-handler.hxx"

#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <http/api-response.hxx>
#include <http/details/http-errors.hxx>

#include <json/value.h>
#include <string>

void ErrorHandler::handleException(
    const std::exception& e, const drogon::HttpRequestPtr&,
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

  // Untyped: the code and the status are the substrate's, the text is the
  // exception's own.
  respCallback(
      ApiResponse::error(HttpErrors::InternalError.withMessage(e.what())));
}

drogon::HttpResponsePtr ErrorHandler::unmatchedRoute(drogon::HttpStatusCode status)
{
  return ApiResponse::error(status == drogon::k405MethodNotAllowed
                                ? HttpErrors::MethodNotAllowed
                                : HttpErrors::PathNotFound);
}
