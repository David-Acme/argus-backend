#include "login-dto.hxx"

#include <drogon/HttpTypes.h>

LoginDto LoginDto::form_multipart(const drogon::MultiPartParser& parser)
{
  auto files = parser.getFilesMap();
  auto it = files.find("image");
  if (it == files.end()) {
    ValidationErrors errs;
    errs["image"] = {"required"};
    throw ValidationException(errs);
  }

  const auto& file = it->second;
  if (file.fileLength() == 0) {
    ValidationErrors errs;
    errs["image"] = {"must not be empty"};
    throw ValidationException(errs);
  }

  if (file.fileLength() > 10 * 1024 * 1024) {
    ValidationErrors errs;
    errs["image"] = {"max 10MB"};
    throw ValidationException(errs);
  }

  LoginDto dto;
  dto.image = std::string(file.fileData(), file.fileLength());
  return dto;
}
