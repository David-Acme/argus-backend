#pragma once

#include <shared/validation/rules.hxx>
#include <shared/validation/validator.hxx>

#define START_VALIDATION(DtoType, objRef) \
  Validator<DtoType> __v;                 \
  using __D = DtoType;                    \
  auto& __d = (objRef);

#define IS_NOT_EMPTY(field) \
  __v.template add<IsNotEmptyRule<__D>>(                  \
      FieldAccessor<__D>{#field,                          \
                         [](const __D& d) -> const std::string& { return d.field; }});

#define IS_BASE64(field) \
  __v.template add<IsBase64Rule<__D>>(                     \
      FieldAccessor<__D>{#field,                           \
                         [](const __D& d) -> const std::string& { return d.field; }});

#define MIN_LENGTH(field, n) \
  __v.template add<MinLengthRule<__D>>(                    \
      FieldAccessor<__D>{#field,                           \
                         [](const __D& d) -> const std::string& { return d.field; }}, n);

#define MAX_LENGTH(field, n) \
  __v.template add<MaxLengthRule<__D>>(                    \
      FieldAccessor<__D>{#field,                           \
                         [](const __D& d) -> const std::string& { return d.field; }}, n);

#define END_VALIDATION() \
  __v.validateOrThrow(__d);
