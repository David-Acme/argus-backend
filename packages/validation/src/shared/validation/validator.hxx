#pragma once

#include <errors/validation-exception.hxx>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    rules_.push_back(std::make_unique<Rule>(std::forward<Args>(args)...));
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
