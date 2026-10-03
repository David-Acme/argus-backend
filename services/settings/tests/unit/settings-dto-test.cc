#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/validation-exception.hxx>
#include <feature/settings/dtos/response-list-settings-dto.hxx>
#include <feature/settings/dtos/response-update-settings-dto.hxx>
#include <feature/settings/dtos/update-settings-dto.hxx>

#include <json/reader.h>

#include <memory>
#include <sstream>
#include <string>

namespace
{
Json::Value parse(const std::string& text)
{
  Json::Value value;
  std::istringstream stream(text);
  Json::CharReaderBuilder builder;
  std::string errors;
  REQUIRE(Json::parseFromStream(builder, stream, &value, &errors));
  return value;
}

ValidationErrors refusalOf(const Json::Value& body)
{
  try {
    (void)UpdateSettingsDto::fromJson(body);
  }
  catch (const ValidationException& error) {
    return error.errors();
  }
  return {};
}

Json::Value changesOf(int count)
{
  Json::Value body(Json::objectValue);
  body["changes"] = Json::Value(Json::arrayValue);
  for (int index = 0; index < count; ++index) {
    Json::Value change(Json::objectValue);
    change["key"] = "tts.key" + std::to_string(index);
    change["value"] = "1";
    body["changes"].append(change);
  }
  return body;
}

OwnerCatalog ttsCatalog()
{
  return {.service = "tts",
          .reachable = true,
          .settings = {{.spec = {.key = "tts.quality",
                                 .group = "voice",
                                 .type = SettingType::Choice,
                                 .level = SettingLevel::Advanced,
                                 .apply = SettingApply::NextSession,
                                 .range = {},
                                 .choices = {"auto", "low"},
                                 .fallback = "auto"},
                        .value = "low"}}};
}
}

TEST_CASE("a well-formed body parses into its changes")
{
  const auto dto = UpdateSettingsDto::fromJson(
      parse(R"({"changes":[{"key":"tts.speed","value":"1.5"},{"key":"tts.quality","value":"low"}]})"));
  REQUIRE(dto.changes.size() == 2);
  CHECK(dto.changes[0].key == "tts.speed");
  CHECK(dto.changes[0].value == "1.5");
  CHECK(dto.changes[1].key == "tts.quality");
  CHECK(dto.changes[1].value == "low");

  CHECK(UpdateSettingsDto::fromJson(changesOf(64)).changes.size() == 64);
}

TEST_CASE("the body is refused when the changes are missing, empty, too many or malformed")
{
  CHECK(refusalOf(parse("{}")).contains("changes"));
  CHECK(refusalOf(parse(R"({"changes":[]})")).contains("changes"));
  CHECK(refusalOf(parse(R"({"changes":"tts.speed"})")).contains("changes"));
  CHECK(refusalOf(changesOf(65)).contains("changes"));
  CHECK(refusalOf(parse(R"({"changes":[{"key":"tts.speed","value":1.5}]})")).contains("changes"));
  CHECK(refusalOf(parse(R"({"changes":[{"key":"","value":"1"}]})")).contains("changes"));
  CHECK(refusalOf(parse(R"({"changes":[{"value":"1"}]})")).contains("changes"));

  Json::Value longKey = changesOf(1);
  longKey["changes"][0]["key"] = std::string(129, 'k');
  CHECK(refusalOf(longKey).contains("changes"));
  longKey["changes"][0]["key"] = std::string(128, 'k');
  CHECK(refusalOf(longKey).empty());

  Json::Value longValue = changesOf(1);
  longValue["changes"][0]["value"] = std::string(513, 'v');
  CHECK(refusalOf(longValue).contains("changes"));
  longValue["changes"][0]["value"] = std::string(512, 'v');
  CHECK(refusalOf(longValue).empty());
}

TEST_CASE("the catalog serializes every field the app reads, in camelCase")
{
  const std::vector<OwnerCatalog> owners{ttsCatalog(), {.service = "stt", .reachable = false, .settings = {}}};
  const Json::Value info = ResponseListSettingsDto{.owners = owners}.toJson();
  REQUIRE(info["owners"].size() == 2);
  const Json::Value& tts = info["owners"][0];
  CHECK(tts["service"].asString() == "tts");
  CHECK(tts["reachable"].asBool());
  REQUIRE(tts["settings"].size() == 1);
  const Json::Value& quality = tts["settings"][0];
  CHECK(quality["key"].asString() == "tts.quality");
  CHECK(quality["group"].asString() == "voice");
  CHECK(quality["type"].asString() == "choice");
  CHECK(quality["level"].asString() == "advanced");
  CHECK(quality["apply"].asString() == "nextSession");
  CHECK(quality["min"].asDouble() == doctest::Approx(0));
  CHECK(quality["max"].asDouble() == doctest::Approx(0));
  CHECK(quality["step"].asDouble() == doctest::Approx(0));
  REQUIRE(quality["choices"].size() == 2);
  CHECK(quality["choices"][1].asString() == "low");
  CHECK(quality["value"].asString() == "low");
  CHECK(quality["fallback"].asString() == "auto");

  const Json::Value& stt = info["owners"][1];
  CHECK(stt["service"].asString() == "stt");
  CHECK_FALSE(stt["reachable"].asBool());
  CHECK(stt["settings"].isArray());
  CHECK(stt["settings"].empty());
}

TEST_CASE("an update answers with the applied keys and the owner's catalog")
{
  const SettingsUpdateOutcome outcome{.applied = {"tts.quality"}, .catalog = ttsCatalog()};
  const Json::Value info = ResponseUpdateSettingsDto{.outcome = outcome}.toJson();
  REQUIRE(info["applied"].size() == 1);
  CHECK(info["applied"][0].asString() == "tts.quality");
  CHECK(info["catalog"]["service"].asString() == "tts");
  CHECK(info["catalog"]["reachable"].asBool());
  CHECK(info["catalog"]["settings"][0]["value"].asString() == "low");
}
