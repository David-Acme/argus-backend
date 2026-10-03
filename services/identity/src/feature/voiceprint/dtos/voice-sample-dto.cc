#include "voice-sample-dto.hxx"

#include <errors/validation-exception.hxx>

VoiceSampleDto
VoiceSampleDto::form_multipart(const drogon::MultiPartParser& parser)
{
  const auto files = parser.getFilesMap();
  const auto found = files.find("sample");
  ValidationErrors errors;
  if (found == files.end())
    errors["sample"] = {"sample is required"};
  else if (found->second.fileLength() == 0)
    errors["sample"] = {"sample must not be empty"};
  else if (found->second.fileLength() > kMaxBytes)
    errors["sample"] = {"sample must not exceed 6MB"};
  if (!errors.empty())
    throw ValidationException(errors);

  VoiceSampleDto dto;
  dto.sample.assign(found->second.fileData(), found->second.fileLength());
  return dto;
}
