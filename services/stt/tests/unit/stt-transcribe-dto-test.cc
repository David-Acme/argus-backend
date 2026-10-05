#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/response-exception.hxx>
#include <feature/stt/dtos/transcribe-dto.hxx>
#include <errors/validation-exception.hxx>

#include <drogon/HttpRequest.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace
{

drogon::HttpRequestPtr pcmRequest(std::string body, const std::string& type = "audio/x-argus-pcm-s16")
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->addHeader("content-type", type);
  req->setBody(std::move(body));
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
  const auto ok = TranscribeDto::fromRequest(pcmRequest(second));
  CHECK(ok.lang == "es");
  CHECK(ok.samples().size() == 16000);

  CHECK_THROWS_AS(static_cast<void>(TranscribeDto::fromRequest(pcmRequest(second, "audio/wav"))),
                  ResponseException);
  CHECK_THROWS_AS(static_cast<void>(TranscribeDto::fromRequest(pcmRequest(std::string(3, '\0')))),
                  ResponseException);
  CHECK(refusedAsValidation(pcmRequest("")));
  CHECK(refusedAsValidation(pcmRequest(std::string(kMaxTranscribeSeconds * 32000 + 2, '\0'))));
  CHECK_FALSE(refusedAsValidation(pcmRequest(std::string(kMaxTranscribeSeconds * 32000, '\0'))));
}

TEST_CASE("samples keep the voice session's int16 to float mapping")
{
  std::string body(4, '\0');
  const int16_t loud = 16384;
  const int16_t low = -32768;
  std::memcpy(body.data(), &loud, sizeof(loud));
  std::memcpy(body.data() + 2, &low, sizeof(low));
  const auto samples = TranscribeDto::fromRequest(pcmRequest(body)).samples();
  REQUIRE(samples.size() == 2);
  CHECK(samples[0] == doctest::Approx(0.5F));
  CHECK(samples[1] == doctest::Approx(-1.0F));
}
