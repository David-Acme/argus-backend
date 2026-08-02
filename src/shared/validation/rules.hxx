#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <regex>
#include <shared/validation/validator.hxx>
#include <string>
#include <vector>

// ---- FieldAccessors ----

template <typename DtoType>
struct FieldAccessor
{
  std::string name;
  std::function<const std::string&(const DtoType&)> get;
};

template <typename DtoType>
struct OptionalFieldAccessor
{
  std::string name;
  std::function<const std::optional<std::string>&(const DtoType&)> get;
};

template <typename DtoType>
struct IntFieldAccessor
{
  std::string name;
  std::function<int64_t(const DtoType&)> get;
};

template <typename DtoType>
struct OptionalIntFieldAccessor
{
  std::string name;
  std::function<const std::optional<int64_t>&(const DtoType&)> get;
};

template <typename DtoType>
struct BoolFieldAccessor
{
  std::string name;
  std::function<bool(const DtoType&)> get;
};

template <typename DtoType, typename ElementType>
struct ArrayFieldAccessor
{
  std::string name;
  std::function<const std::vector<ElementType>&(const DtoType&)> get;
};

// ---- Presence rules ----

template <typename DtoType>
class IsNotEmptyRule : public Validator<DtoType>::IRule
{
public:
  explicit IsNotEmptyRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
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
class IsOptionalNotEmptyRule : public Validator<DtoType>::IRule
{
public:
  explicit IsOptionalNotEmptyRule(OptionalFieldAccessor<DtoType> f)
      : accessor_(std::move(f))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.has_value() && v->empty())
      return accessor_.name + " must not be empty";
    return std::nullopt;
  }

private:
  OptionalFieldAccessor<DtoType> accessor_;
};

// ---- Character type rules ----

template <typename DtoType>
class IsAlphaRule : public Validator<DtoType>::IRule
{
public:
  explicit IsAlphaRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    static const std::regex re(R"(^[a-zA-Z]+$)");
    if (!std::regex_match(v, re))
      return accessor_.name + " must contain only letters";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsAlnumRule : public Validator<DtoType>::IRule
{
public:
  explicit IsAlnumRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    static const std::regex re(R"(^[a-zA-Z0-9]+$)");
    if (!std::regex_match(v, re))
      return accessor_.name + " must contain only letters and digits";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class HasNoSpacesRule : public Validator<DtoType>::IRule
{
public:
  explicit HasNoSpacesRule(FieldAccessor<DtoType> f) : accessor_(std::move(f))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.find(' ') != std::string::npos || v.find('\t') != std::string::npos)
      return accessor_.name + " must not contain spaces";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

// ---- Format rules ----

template <typename DtoType>
class IsEmailRule : public Validator<DtoType>::IRule
{
public:
  explicit IsEmailRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    static const std::regex re(R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)");
    if (!std::regex_match(v, re))
      return accessor_.name + " must be a valid email";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsUuidRule : public Validator<DtoType>::IRule
{
public:
  explicit IsUuidRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    static const std::regex re(
        R"(^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
    if (!std::regex_match(v, re))
      return accessor_.name + " must be a valid UUID";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsUrlRule : public Validator<DtoType>::IRule
{
public:
  explicit IsUrlRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    static const std::regex re(R"(^https?://.+\..+)");
    if (!std::regex_match(v, re))
      return accessor_.name + " must be a valid URL";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsHexRule : public Validator<DtoType>::IRule
{
public:
  explicit IsHexRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    if (v.size() % 2 != 0 || !std::all_of(v.begin(), v.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F');
        }))
      return accessor_.name + " must be a valid hex string";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsSlugRule : public Validator<DtoType>::IRule
{
public:
  explicit IsSlugRule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    static const std::regex re(R"(^[a-z0-9]+(?:-[a-z0-9]+)*$)");
    if (!std::regex_match(v, re))
      return accessor_.name + " must be a valid slug";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsBase64Rule : public Validator<DtoType>::IRule
{
public:
  explicit IsBase64Rule(FieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
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
class MatchesRegexRule : public Validator<DtoType>::IRule
{
public:
  MatchesRegexRule(FieldAccessor<DtoType> f, std::string pattern,
                   std::string message = "does not match pattern")
      : accessor_(std::move(f)), pattern_(std::move(pattern)),
        message_(std::move(message))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.empty())
      return std::nullopt;
    const std::regex re(pattern_);
    if (!std::regex_match(v, re))
      return accessor_.name + " " + message_;
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
  std::string pattern_;
  std::string message_;
};

// ---- Inclusion rules ----

template <typename DtoType>
class IsInRule : public Validator<DtoType>::IRule
{
public:
  IsInRule(FieldAccessor<DtoType> f, std::initializer_list<std::string> allowed)
      : accessor_(std::move(f)), allowed_(allowed)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (std::find(allowed_.begin(), allowed_.end(), v) == allowed_.end())
      return accessor_.name + " must be one of the allowed values";
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor_;
  std::vector<std::string> allowed_;
};

// ---- Length rules ----

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

template <typename DtoType>
class MinLengthOptionalRule : public Validator<DtoType>::IRule
{
public:
  MinLengthOptionalRule(OptionalFieldAccessor<DtoType> f, size_t min)
      : accessor_(std::move(f)), min_(min)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.has_value() && v->length() < min_)
      return accessor_.name + " must be at least " + std::to_string(min_) +
             " characters";
    return std::nullopt;
  }

private:
  OptionalFieldAccessor<DtoType> accessor_;
  size_t min_;
};

template <typename DtoType>
class MaxLengthOptionalRule : public Validator<DtoType>::IRule
{
public:
  MaxLengthOptionalRule(OptionalFieldAccessor<DtoType> f, size_t max)
      : accessor_(std::move(f)), max_(max)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.has_value() && v->length() > max_)
      return accessor_.name + " must be at most " + std::to_string(max_) +
             " characters";
    return std::nullopt;
  }

private:
  OptionalFieldAccessor<DtoType> accessor_;
  size_t max_;
};

// ---- Numeric rules ----

template <typename DtoType>
class IsPositiveRule : public Validator<DtoType>::IRule
{
public:
  IsPositiveRule(IntFieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj) <= 0)
      return accessor_.name + " must be positive";
    return std::nullopt;
  }

private:
  IntFieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsNonNegativeRule : public Validator<DtoType>::IRule
{
public:
  IsNonNegativeRule(IntFieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj) < 0)
      return accessor_.name + " must be non-negative";
    return std::nullopt;
  }

private:
  IntFieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class MinIntRule : public Validator<DtoType>::IRule
{
public:
  MinIntRule(IntFieldAccessor<DtoType> f, int64_t min)
      : accessor_(std::move(f)), min_(min)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj) < min_)
      return accessor_.name + " must be at least " + std::to_string(min_);
    return std::nullopt;
  }

private:
  IntFieldAccessor<DtoType> accessor_;
  int64_t min_;
};

template <typename DtoType>
class MaxIntRule : public Validator<DtoType>::IRule
{
public:
  MaxIntRule(IntFieldAccessor<DtoType> f, int64_t max)
      : accessor_(std::move(f)), max_(max)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj) > max_)
      return accessor_.name + " must be at most " + std::to_string(max_);
    return std::nullopt;
  }

private:
  IntFieldAccessor<DtoType> accessor_;
  int64_t max_;
};

template <typename DtoType>
class BetweenRule : public Validator<DtoType>::IRule
{
public:
  BetweenRule(IntFieldAccessor<DtoType> f, int64_t min, int64_t max)
      : accessor_(std::move(f)), min_(min), max_(max)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    auto v = accessor_.get(obj);
    if (v < min_ || v > max_)
      return accessor_.name + " must be between " + std::to_string(min_) +
             " and " + std::to_string(max_);
    return std::nullopt;
  }

private:
  IntFieldAccessor<DtoType> accessor_;
  int64_t min_;
  int64_t max_;
};

// ---- Cross-field rules ----

template <typename DtoType>
class EqualsFieldRule : public Validator<DtoType>::IRule
{
public:
  EqualsFieldRule(FieldAccessor<DtoType> f1, FieldAccessor<DtoType> f2,
                  std::string f2name)
      : accessor1_(std::move(f1)), accessor2_(std::move(f2)),
        field2_(std::move(f2name))
  {
  }
  std::string field() const override { return accessor1_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor1_.get(obj) != accessor2_.get(obj))
      return accessor1_.name + " must equal " + field2_;
    return std::nullopt;
  }

private:
  FieldAccessor<DtoType> accessor1_;
  FieldAccessor<DtoType> accessor2_;
  std::string field2_;
};

// ---- Array rules ----

template <typename DtoType, typename ElementType>
class ArrayNotEmptyRule : public Validator<DtoType>::IRule
{
public:
  explicit ArrayNotEmptyRule(ArrayFieldAccessor<DtoType, ElementType> f)
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
  ArrayFieldAccessor<DtoType, ElementType> accessor_;
};

template <typename DtoType, typename ElementType>
class MinElementsRule : public Validator<DtoType>::IRule
{
public:
  MinElementsRule(ArrayFieldAccessor<DtoType, ElementType> f, size_t min)
      : accessor_(std::move(f)), min_(min)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj).size() < min_)
      return accessor_.name + " must have at least " + std::to_string(min_) +
             " elements";
    return std::nullopt;
  }

private:
  ArrayFieldAccessor<DtoType, ElementType> accessor_;
  size_t min_;
};

template <typename DtoType, typename ElementType>
class MaxElementsRule : public Validator<DtoType>::IRule
{
public:
  MaxElementsRule(ArrayFieldAccessor<DtoType, ElementType> f, size_t max)
      : accessor_(std::move(f)), max_(max)
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj).size() > max_)
      return accessor_.name + " must have at most " + std::to_string(max_) +
             " elements";
    return std::nullopt;
  }

private:
  ArrayFieldAccessor<DtoType, ElementType> accessor_;
  size_t max_;
};

// ---- Timestamp rules ----

template <typename DtoType>
class IsValidTimestampRule : public Validator<DtoType>::IRule
{
public:
  IsValidTimestampRule(IntFieldAccessor<DtoType> f) : accessor_(std::move(f)) {}
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj) == 0)
      return accessor_.name + " must be a valid timestamp";
    return std::nullopt;
  }

private:
  IntFieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsPositiveTimestampRule : public Validator<DtoType>::IRule
{
public:
  IsPositiveTimestampRule(IntFieldAccessor<DtoType> f) : accessor_(std::move(f))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    if (accessor_.get(obj) <= 0)
      return accessor_.name + " must be a positive timestamp";
    return std::nullopt;
  }

private:
  IntFieldAccessor<DtoType> accessor_;
};

template <typename DtoType>
class IsPositiveTimestampOptionalRule : public Validator<DtoType>::IRule
{
public:
  IsPositiveTimestampOptionalRule(OptionalIntFieldAccessor<DtoType> f)
      : accessor_(std::move(f))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    const auto& v = accessor_.get(obj);
    if (v.has_value() && *v <= 0)
      return accessor_.name + " must be a positive timestamp";
    return std::nullopt;
  }

private:
  OptionalIntFieldAccessor<DtoType> accessor_;
};

// ---- Boolean rules ----

template <typename DtoType>
class IsBooleanRule : public Validator<DtoType>::IRule
{
public:
  explicit IsBooleanRule(BoolFieldAccessor<DtoType> f) : accessor_(std::move(f))
  {
  }
  std::string field() const override { return accessor_.name; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    (void)accessor_.get(obj);
    return std::nullopt;
  }

private:
  BoolFieldAccessor<DtoType> accessor_;
};

// ---- Custom rules ----

template <typename DtoType>
class LambdaRule : public Validator<DtoType>::IRule
{
public:
  LambdaRule(std::string fieldName,
             std::function<std::optional<std::string>(const DtoType&)> fn)
      : field_(std::move(fieldName)), fn_(std::move(fn))
  {
  }
  std::string field() const override { return field_; }
  std::optional<std::string> validate(const DtoType& obj) const override
  {
    return fn_(obj);
  }

private:
  std::string field_;
  std::function<std::optional<std::string>(const DtoType&)> fn_;
};
