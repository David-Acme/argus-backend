#pragma once

#include <shared/validation/rules.hxx>
#include <shared/validation/validator.hxx>

#define START_VALIDATION(DtoType, objRef) \
  Validator<DtoType> __v;                 \
  using __D = DtoType;                    \
  auto& __d = (objRef);

// ---- Presence ----

#define IS_NOT_EMPTY(field)                                            \
  __v.template add<IsNotEmptyRule<__D>>(                               \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define IS_NOT_EMPTY_OPTIONAL(field)                                  \
  __v.template add<IsOptionalNotEmptyRule<__D>>(                      \
      OptionalFieldAccessor<__D>{#field,                              \
                                 [](const __D& d)                     \
                                     -> const std::optional<          \
                                         std::string>& { return d.field; }});

// ---- Character type ----

#define IS_ALPHA(field)                                                \
  __v.template add<IsAlphaRule<__D>>(                                  \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define IS_ALNUM(field)                                                \
  __v.template add<IsAlnumRule<__D>>(                                  \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define HAS_NO_SPACES(field)                                           \
  __v.template add<HasNoSpacesRule<__D>>(                              \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

// ---- Format ----

#define IS_EMAIL(field)                                                \
  __v.template add<IsEmailRule<__D>>(                                  \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define IS_UUID(field)                                                 \
  __v.template add<IsUuidRule<__D>>(                                   \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define IS_URL(field)                                                  \
  __v.template add<IsUrlRule<__D>>(                                    \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define IS_HEX(field)                                                  \
  __v.template add<IsHexRule<__D>>(                                    \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define IS_SLUG(field)                                                 \
  __v.template add<IsSlugRule<__D>>(                                   \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define IS_BASE64(field)                                               \
  __v.template add<IsBase64Rule<__D>>(                                 \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }});

#define MATCHES_REGEX(field, pattern, message)                         \
  __v.template add<MatchesRegexRule<__D>>(                             \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }},                        \
      pattern, message);

// ---- Inclusion ----

#define IS_IN(field, ...)                                              \
  __v.template add<IsInRule<__D>>(                                     \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }},                        \
      std::initializer_list<std::string>{__VA_ARGS__});

// ---- Length ----

#define MIN_LENGTH(field, n)                                           \
  __v.template add<MinLengthRule<__D>>(                                \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }},                        \
      n);

#define MAX_LENGTH(field, n)                                           \
  __v.template add<MaxLengthRule<__D>>(                                \
      FieldAccessor<__D>{#field, [](const __D& d) -> const std::string& \
                          { return d.field; }},                        \
      n);

#define MIN_LENGTH_OPTIONAL(field, n)                                  \
  __v.template add<MinLengthOptionalRule<__D>>(                        \
      OptionalFieldAccessor<__D>{#field,                               \
                                 [](const __D& d)                      \
                                     -> const std::optional<           \
                                         std::string>& { return d.field; }}, \
      n);

#define MAX_LENGTH_OPTIONAL(field, n)                                  \
  __v.template add<MaxLengthOptionalRule<__D>>(                        \
      OptionalFieldAccessor<__D>{#field,                               \
                                 [](const __D& d)                      \
                                     -> const std::optional<           \
                                         std::string>& { return d.field; }}, \
      n);

// ---- Numeric ----

#define IS_POSITIVE(field)                                             \
  __v.template add<IsPositiveRule<__D>>(                               \
      IntFieldAccessor<__D>{#field,                                    \
                            [](const __D& d) -> int64_t { return d.field; }});

#define IS_NON_NEGATIVE(field)                                         \
  __v.template add<IsNonNegativeRule<__D>>(                            \
      IntFieldAccessor<__D>{#field,                                    \
                            [](const __D& d) -> int64_t { return d.field; }});

#define MIN_INT(field, n)                                              \
  __v.template add<MinIntRule<__D>>(                                   \
      IntFieldAccessor<__D>{#field,                                    \
                            [](const __D& d) -> int64_t { return d.field; }}, \
      n);

#define MAX_INT(field, n)                                              \
  __v.template add<MaxIntRule<__D>>(                                   \
      IntFieldAccessor<__D>{#field,                                    \
                            [](const __D& d) -> int64_t { return d.field; }}, \
      n);

#define BETWEEN(field, min, max)                                       \
  __v.template add<BetweenRule<__D>>(                                  \
      IntFieldAccessor<__D>{#field,                                    \
                            [](const __D& d) -> int64_t { return d.field; }}, \
      min, max);

// ---- Cross-field ----

#define EQUALS_FIELD(field1, field2)                                   \
  __v.template add<EqualsFieldRule<__D>>(                              \
      FieldAccessor<__D>{#field1,                                      \
                          [](const __D& d) -> const std::string& { return d.field1; }}, \
      FieldAccessor<__D>{#field2,                                      \
                          [](const __D& d) -> const std::string& { return d.field2; }}, \
      std::string{#field2});

// ---- Array ----

#define ARRAY_NOT_EMPTY(field, ElementType)                            \
  __v.template add<ArrayNotEmptyRule<__D, ElementType>>(               \
      ArrayFieldAccessor<__D, ElementType>{#field,                     \
                                           [](const __D& d)            \
                                               -> const std::vector<   \
                                                   ElementType>& { return d.field; }});

#define MIN_ELEMENTS(field, ElementType, n)                            \
  __v.template add<MinElementsRule<__D, ElementType>>(                 \
      ArrayFieldAccessor<__D, ElementType>{#field,                     \
                                           [](const __D& d)            \
                                               -> const std::vector<   \
                                                   ElementType>& { return d.field; }}, \
      n);

#define MAX_ELEMENTS(field, ElementType, n)                            \
  __v.template add<MaxElementsRule<__D, ElementType>>(                 \
      ArrayFieldAccessor<__D, ElementType>{#field,                     \
                                           [](const __D& d)            \
                                               -> const std::vector<   \
                                                   ElementType>& { return d.field; }}, \
      n);

// ---- Timestamp ----

#define IS_VALID_TIMESTAMP(field)                                      \
  __v.template add<IsValidTimestampRule<__D>>(                         \
      IntFieldAccessor<__D>{#field,                                    \
                            [](const __D& d) -> int64_t { return d.field; }});

#define IS_POSITIVE_TIMESTAMP(field)                                   \
  __v.template add<IsPositiveTimestampRule<__D>>(                      \
      IntFieldAccessor<__D>{#field,                                    \
                            [](const __D& d) -> int64_t { return d.field; }});

// ---- Boolean ----

#define IS_BOOLEAN(field)                                              \
  __v.template add<IsBooleanRule<__D>>(                                \
      BoolFieldAccessor<__D>{#field,                                   \
                             [](const __D& d) -> bool { return d.field; }});

// ---- Custom ----

#define CUSTOM_LAMBDA(field, fn)                                       \
  __v.template add<LambdaRule<__D>>(std::string{#field}, fn);

// ----

#define END_VALIDATION() __v.validateOrThrow(__d);
