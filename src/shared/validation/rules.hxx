#pragma once

#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <shared/validation/validator.hxx>

// ---- FieldAccessor: name + getter ----

template <typename DtoType>
struct FieldAccessor
{
  std::string name;
  std::function<const std::string&(const DtoType&)> get;
};

// ---- Concrete rules (inherit Validator<T>::IRule) ----

template <typename DtoType>
class IsNotEmptyRule : public Validator<DtoType>::IRule
{
public:
  explicit IsNotEmptyRule(FieldAccessor<DtoType> f)
      : accessor_(std::move(f))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj).empty())
      return accessor_.name + " must not be empty";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsBase64Rule : public Validator<DtoType>::IRule
{
public:
  explicit IsBase64Rule(FieldAccessor<DtoType> f)
      : accessor_(std::move(f))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty()) return std::nullopt;
    if (v.size() % 4 != 0)
      return accessor_.name + " must be a valid base64 string";
    static const std::regex re(R"(^[A-Za-z0-9+/]*={0,2}$)");
    if (!std::regex_match(v, re))
      return accessor_.name + " must be a valid base64 string";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class MinLengthRule : public Validator<DtoType>::IRule
{
public:
  MinLengthRule(FieldAccessor<DtoType> f, size_t min)
      : accessor_(std::move(f)), min_(min)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.length() < min_)
      return accessor_.name + " must be at least " + std::to_string(min_) +
             " characters";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
  size_t min_;
};

template <typename DtoType>
class MaxLengthRule : public Validator<DtoType>::IRule
{
public:
  MaxLengthRule(FieldAccessor<DtoType> f, size_t max)
      : accessor_(std::move(f)), max_(max)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.length() > max_)
      return accessor_.name + " must be at most " + std::to_string(max_) +
             " characters";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
  size_t max_;
};
