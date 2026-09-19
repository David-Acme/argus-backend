#include "register-dto.hxx"

#include <drogon/HttpTypes.h>
#include <string>

RegisterDto RegisterDto::form_multipart(const drogon::MultiPartParser& parser)
{
  auto files = parser.getFilesMap();
  auto it = files.find("image");
  if (it == files.end()) {
    ValidationErrors errs;
    errs["image"] = {"image is required"};
    throw ValidationException(errs);
  }

  const auto& file = it->second;
  if (file.fileLength() == 0) {
    ValidationErrors errs;
    errs["image"] = {"image must not be empty"};
    throw ValidationException(errs);
  }

  if (file.fileLength() > 10 * 1024 * 1024) {
    ValidationErrors errs;
    errs["image"] = {"image must not exceed 10MB"};
    throw ValidationException(errs);
  }

  RegisterDto dto;
  dto.image = std::string(file.fileData(), file.fileLength());

  auto params = parser.getParameters();
  auto nameIt = params.find("name");
  if (nameIt != params.end())
    dto.name = std::string(nameIt->second);
  auto inviteIt = params.find("inviteCode");
  if (inviteIt != params.end())
    dto.inviteCode = std::string(inviteIt->second);
  auto langIt = params.find("lang");
  if (langIt != params.end())
    dto.lang = std::string(langIt->second);

  START_VALIDATION(RegisterDto, dto)
  MAX_LENGTH(name, 120)
  MAX_LENGTH(inviteCode, 64)
  CUSTOM_LAMBDA(lang, [](const RegisterDto& d) -> std::optional<std::string> {
    if (d.lang.empty() || d.lang == "es" || d.lang == "en")
      return std::nullopt;
    return "lang must be es or en";
  })
  END_VALIDATION()

  return dto;
}