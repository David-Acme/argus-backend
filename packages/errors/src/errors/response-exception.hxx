#pragma once

#include "error-definition.hxx"

#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// One refusal as it travels: what a single error looks like once it is off the
// catalog and on its way to the wire.
struct ResponseError
{
  std::string code;
  std::string message;
};

using ResponseErrors = std::variant<ResponseError, std::vector<ResponseError>>;

// A refusal whose code was already a string: it came off the wire, so there was
// no catalog entry to look up and only the status was known.
struct ResponseExceptionInput
{
  std::string message;
  int statusCode{400};
  std::string errorCode{"ERROR"};
};

// The one way to refuse (architecture plan section 4.7). A handler throws and
// the shared advice formats it; nothing builds a response by hand. The
// constructors are the ways a refusal arises: from a catalog definition, from a
// list of wire records, or from a bare text no catalog named.
class ResponseException : public std::runtime_error
{
public:
  explicit ResponseException(const ErrorDefinition& error);
  ResponseException(int statusCode, const ErrorDefinition& error);
  ResponseException(int statusCode, std::vector<ResponseError> errors);
  explicit ResponseException(std::string message);
  explicit ResponseException(const ResponseExceptionInput& input);

  int statusCode() const noexcept;
  const std::string& errorCode() const noexcept;
  const ResponseErrors& errors() const noexcept;

private:
  int statusCode_;
  ResponseErrors errors_;
};
