#include "enroll-voiceprint-dto.hxx"

#include <feature/voiceprint/dtos/voice-sample-dto.hxx>

namespace
{
constexpr size_t kMaxSamples = 10;
constexpr size_t kMaxFaceBytes = size_t{10} * 1024 * 1024;

std::string parameter(const drogon::MultiPartParser& parser,
                      const std::string& name)
{
  const auto& params = parser.getParameters();
  const auto found = params.find(name);
  return found == params.end() ? std::string() : std::string(found->second);
}
}

EnrollVoiceprintDto
EnrollVoiceprintDto::form_multipart(const drogon::MultiPartParser& parser)
{
  EnrollVoiceprintDto dto;
  bool oversized = false;
  for (const auto& file : parser.getFiles()) {
    if (file.getItemName() == "samples") {
      oversized = oversized || file.fileLength() == 0 ||
                  file.fileLength() > VoiceSampleDto::kMaxBytes;
      dto.samples.emplace_back(file.fileData(), file.fileLength());
    }
    else if (file.getItemName() == "face") {
      oversized = oversized || file.fileLength() > kMaxFaceBytes;
      dto.face.assign(file.fileData(), file.fileLength());
    }
  }
  dto.consent = parameter(parser, "consent");
  dto.consentVersion = parameter(parser, "consentVersion");
  dto.challengeId = parameter(parser, "challengeId");

  START_VALIDATION(EnrollVoiceprintDto, dto)
  CUSTOM_LAMBDA(
      samples,
      [oversized](
          const EnrollVoiceprintDto& value) -> std::optional<std::string> {
        if (value.samples.empty() || value.samples.size() > kMaxSamples)
          return "between 1 and 10 samples are required";
        if (oversized)
          return "every sample must be a non-empty WAV of at most 6MB";
        return std::nullopt;
      })
  IS_IN(consent, "true")
  IS_NOT_EMPTY(consentVersion)
  MAX_LENGTH(consentVersion, 64)
  IS_NOT_EMPTY(challengeId)
  MAX_LENGTH(challengeId, 128)
  END_VALIDATION()
  return dto;
}
