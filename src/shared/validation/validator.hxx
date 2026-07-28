#pragma once

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using ValidationErrors = std::map<std::string, std::vector<std::string>>;

class ValidationException : public std::runtime_error
{
public:
  explicit ValidationException(const ValidationErrors& errors,
                               int statusCode = 422)
      : std::runtime_error("Validation failed"), errors_(errors),
        statusCode_(statusCode)
  {
  }

  const ValidationErrors& errors() const { return errors_; }
  int statusCode() const { return statusCode_; }

private:
  ValidationErrors errors_;
  int statusCode_;
};

template <typename DtoType>
class Validator
{
public:
  struct IRule
  {
    virtual ~IRule() = default;
    virtual std::optional<std::string> validate(const DtoType&) const = 0;
    virtual std::string field() const = 0;
  };

  template <typename Rule, typename... Args>
  void add(Args&&... args)
  {
    rules_.push_back(
        std::make_unique<Rule>(std::forward<Args>(args)...));
  }

  void validateOrThrow(const DtoType& obj) const
  {
    ValidationErrors errors;
    for (const auto& rule : rules_) {
      auto err = rule->validate(obj);
      if (err)
        errors[rule->field()].push_back(*err);
    }
    if (!errors.empty())
      throw ValidationException(errors);
  }

private:
  std::vector<std::unique_ptr<IRule>> rules_;
};
