#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/response-exception.hxx>
#include <feature/stt/dtos/transcribe-dto.hxx>
#include <errors/validation-exception.hxx>

#include <drogon/HttpRequest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace
{

constexpr const char* kPcm16 = "audio/x-argus-pcm-s16";

struct PcmUpload
{
  std::string body;
  std::string type;
};

drogon::HttpRequestPtr pcmRequest(PcmUpload upload)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->addHeader("content-type", upload.type);
  req->setBody(std::move(upload.body));
  req->setParameter("lang", "es");
  return req;
}

bool refusedAsValidation(const drogon::HttpRequestPtr& req)
{
  try {
    static_cast<void>(TranscribeDto::fromRequest(req));
  }
  catch (const ValidationException&) {
    return true;
  }
  return false;
}

}

TEST_CASE("the transcribe body is pcm16, aligned and at most two minutes long")
{
  const std::string second(32000, '\0');
  const auto ok = TranscribeDto::fromRequest(pcmRequest({.body = second, .type = kPcm16}));
  CHECK(ok.lang == "es");
  CHECK(ok.samples().size() == 16000);

  CHECK_THROWS_AS(static_cast<void>(TranscribeDto::fromRequest(pcmRequest({.body = second, .type = "audio/wav"}))),
                  ResponseException);
  CHECK_THROWS_AS(static_cast<void>(TranscribeDto::fromRequest(pcmRequest({.body = std::string(3, '\0'), .type = kPcm16}))),
                  ResponseException);
  CHECK(refusedAsValidation(pcmRequest({.body = "", .type = kPcm16})));
  CHECK(refusedAsValidation(pcmRequest({.body = std::string(kMaxTranscribeSeconds * 32000 + 2, '\0'), .type = kPcm16})));
  CHECK_FALSE(refusedAsValidation(pcmRequest({.body = std::string(kMaxTranscribeSeconds * 32000, '\0'), .type = kPcm16})));
}

TEST_CASE("samples keep the voice session's int16 to float mapping")
{
  std::string body(4, '\0');
  const int16_t loud = 16384;
  const int16_t low = -32768;
  std::memcpy(body.data(), &loud, sizeof(loud));
  std::memcpy(body.data() + 2, &low, sizeof(low));
  const auto samples = TranscribeDto::fromRequest(pcmRequest({.body = body, .type = kPcm16})).samples();
  REQUIRE(samples.size() == 2);
  CHECK(samples[0] == doctest::Approx(0.5F));
  CHECK(samples[1] == doctest::Approx(-1.0F));
}
