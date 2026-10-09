#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/gliner-extractor.hxx>
#include <feature/llm/services/turn/onnx-session.hxx>
#include <feature/llm/services/turn/slots.hxx>
#include <feature/llm/services/turn/sp-tokenizer.hxx>
#include <feature/memory/services/extract/call-time.hxx>

#include <mcp/schema.hxx>
#include <text/iso-time.hxx>

#include <json/reader.h>
#include <json/value.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

struct Pilot
{
  fs::path dir;
  std::string pin;
};

std::optional<Pilot> pilotBundle()
{
  const char* dir = std::getenv("ARGUS_GLINER_BUNDLE");
  const char* pin = std::getenv("ARGUS_GLINER_PIN");
  if (dir == nullptr || pin == nullptr || !fs::exists(fs::path(dir) / "model.onnx"))
    return std::nullopt;
  return Pilot{.dir = fs::path(dir), .pin = pin};
}

Json::Value readJson(const char* path)
{
  std::ifstream in(path);
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  Json::parseFromStream(builder, in, &root, &errors);
  return root;
}

std::vector<std::string> stringsOf(const Json::Value& node)
{
  std::vector<std::string> out;
  for (const Json::Value& item : node)
    out.push_back(item.asString());
  return out;
}

std::vector<std::int64_t> integersOf(const Json::Value& node)
{
  std::vector<std::int64_t> out;
  for (const Json::Value& item : node)
    out.push_back(static_cast<std::int64_t>(item.asInt64()));
  return out;
}

int64_t utc(int day, int hour, int minute = 0)
{
  std::tm at{};
  at.tm_year = 2026 - 1900;
  at.tm_mon = 10 - 1;
  at.tm_mday = day;
  at.tm_hour = hour;
  at.tm_min = minute;
  return static_cast<int64_t>(timegm(&at));
}

int64_t fixedNow()
{
  setenv("TZ", "UTC", 1);
  tzset();
  return utc(7, 15, 20);
}

struct World
{
  argus::mcp::ToolSpec spec;
  tools::ToolContext context;
  ModuleSnapshot modules;

  World()
  {
    Json::Value properties(Json::objectValue);
    properties["title"] = argus::mcp::schema::text();
    Json::Value start(Json::objectValue);
    start["type"] = "string";
    start["format"] = "date-time";
    properties["starts_at"] = start;
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    schema["properties"] = properties;
    Json::Value required(Json::arrayValue);
    required.append("title");
    required.append("starts_at");
    schema["required"] = required;
    spec = {.name = "calendar.create_event",
            .title = "",
            .description = "probe",
            .inputSchema = schema,
            .annotations = {},
            .module = "core",
            .capability = "agenda.write"};
    context = {.userId = 7,
               .role = UserRole::Owner,
               .lang = "es",
               .sessionId = "gliner-runtime",
               .channel = "tool_result",
               .utterance = {},
               .decided = false,
               .turn = 1,
               .emitAction = {}};
  }
};

std::vector<turn::GlinerSpan> spansOf(turn::GlinerModel& model, const Json::Value& item)
{
  return model.spans({.text = item["text"].asString(),
                      .lang = item["lang"].asString(),
                      .type = item["type"].asString()});
}

std::vector<const turn::GlinerSpan*> fieldSpans(const std::vector<turn::GlinerSpan>& spans, const std::string& field)
{
  std::vector<const turn::GlinerSpan*> out;
  for (const turn::GlinerSpan& span : spans)
    if (span.field == field)
      out.push_back(&span);
  return out;
}
}

TEST_CASE("the GLiNER word split is gliner2's on every frozen case, terminator included")
{
  const Json::Value fixture = readJson(ARGUS_TEST_GLINER_PARITY);
  std::size_t rows = 0;
  std::size_t mismatches = 0;
  std::size_t words = 0;
  for (const Json::Value& item : fixture["cases"]) {
    const std::string document = turn::glinerDocument(item["text"].asString());
    const std::vector<turn::GlinerWord> split = turn::glinerSplitWords(document);
    const std::vector<std::int64_t> starts = integersOf(item["wordStarts"]);
    const std::vector<std::int64_t> ends = integersOf(item["wordEnds"]);
    if (split.size() != starts.size() || split.size() != ends.size()) {
      ++mismatches;
      ++rows;
      continue;
    }
    for (std::size_t at = 0; at < split.size(); ++at) {
      if (split[at].begin != static_cast<std::size_t>(starts[at]) || split[at].end != static_cast<std::size_t>(ends[at])) {
        ++mismatches;
        break;
      }
    }
    words += split.size();
    ++rows;
  }
  MESSAGE("gliner word split: " << rows << " rows, " << words << " words, " << mismatches << " rows differing");
  CHECK(rows == static_cast<std::size_t>(fixture["cases"].size()));
  CHECK(mismatches == 0);
}

TEST_CASE("the GLiNER prompt reproduces the reference token ids and the three routing tensors")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no GLiNER pilot bundle at ARGUS_GLINER_BUNDLE and ARGUS_GLINER_PIN; the prompt parity did not run");
    return;
  }
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(pilot->dir / "tokenizer" / "tokenizer.json"));
  const Json::Value fixture = readJson(ARGUS_TEST_GLINER_PARITY);
  std::size_t rows = 0;
  std::size_t tokenMismatches = 0;
  std::size_t indexMismatches = 0;
  for (const Json::Value& item : fixture["cases"]) {
    const std::vector<std::string> fields = stringsOf(item["fields"]);
    const std::string document = turn::glinerDocument(item["text"].asString());
    const turn::GlinerPrompt prompt = turn::glinerPrompt(
        {.tokenizer = tokenizer, .document = document, .type = item["type"].asString(), .fields = fields});
    if (prompt.ids != integersOf(item["inputIds"]))
      ++tokenMismatches;
    if (prompt.wordIndices != integersOf(item["wordIndices"]) ||
        prompt.wordMask != integersOf(item["wordMask"]) ||
        prompt.queryIndices != integersOf(item["queryIndices"]) ||
        prompt.queryMask != integersOf(item["queryMask"]))
      ++indexMismatches;
    ++rows;
  }
  MESSAGE("gliner prompt: " << rows << " rows, " << tokenMismatches << " token id rows differing, " << indexMismatches
                            << " routing rows differing");
  CHECK(rows == static_cast<std::size_t>(fixture["cases"].size()));
  CHECK(tokenMismatches == 0);
  CHECK(indexMismatches == 0);
}

TEST_CASE("the real GLiNER bundle decodes the spans the reference holds")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no GLiNER pilot bundle at ARGUS_GLINER_BUNDLE and ARGUS_GLINER_PIN; the span parity did not run");
    return;
  }
  const turn::BundleLoader bundle({.dir = pilot->dir, .pin = pilot->pin, .kind = turn::BundleKind::Extractor});
  REQUIRE(bundle.valid());
  REQUIRE(bundle.thresholds().present);
  CHECK(bundle.thresholds().threshold == doctest::Approx(0.1));
  CHECK(bundle.thresholds().maxSpanWidth == 16);
  const std::unique_ptr<turn::GlinerModel> model = turn::openGlinerModel(bundle, turn::OnnxOptions{});
  REQUIRE(model->status() == turn::EngineStatus::Ready);
  const Json::Value fixture = readJson(ARGUS_TEST_GLINER_PARITY);
  std::size_t rows = 0;
  std::size_t mismatches = 0;
  double worst = 0.0;
  for (const Json::Value& item : fixture["cases"]) {
    const std::vector<turn::GlinerSpan> spans = spansOf(*model, item);
    for (const std::string& field : stringsOf(item["fields"])) {
      const std::vector<const turn::GlinerSpan*> got = fieldSpans(spans, field);
      const Json::Value& expected = item["reference"][field];
      if (got.size() != expected.size()) {
        ++mismatches;
        continue;
      }
      for (Json::ArrayIndex at = 0; at < expected.size(); ++at) {
        if (got[at]->text != expected[at]["text"].asString() ||
            got[at]->begin != static_cast<std::size_t>(expected[at]["start"].asUInt()) ||
            got[at]->end != static_cast<std::size_t>(expected[at]["end"].asUInt()))
          ++mismatches;
        worst = std::max(worst, std::abs(got[at]->score - expected[at]["confidence"].asDouble()));
      }
    }
    ++rows;
  }
  MESSAGE("gliner spans: " << rows << " rows, " << mismatches << " fields differing, worst confidence gap " << worst);
  CHECK(rows == static_cast<std::size_t>(fixture["cases"].size()));
  CHECK(mismatches == 0);
  CHECK(worst <= 2e-3);
}

TEST_CASE("the GLiNER extractor fills a tool call whose date the resolver reads")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no GLiNER pilot bundle at ARGUS_GLINER_BUNDLE and ARGUS_GLINER_PIN; the end-to-end turn did not run");
    return;
  }
  const turn::BundleLoader bundle({.dir = pilot->dir, .pin = pilot->pin, .kind = turn::BundleKind::Extractor});
  REQUIRE(bundle.valid());
  World world;
  const int64_t now = fixedNow();
  const std::string utterance = "agéndame una reunión con Carlos mañana a las cinco de la tarde";
  world.context.utterance = utterance;
  world.context.now = now;
  const turn::GlinerExtractor text({.model = turn::openGlinerModel(bundle, turn::OnnxOptions{}),
                                    .fallback = nullptr,
                                    .thresholds = bundle.thresholds()});
  const std::vector<std::string> fields{"title", "starts_at"};
  const slots::Filled filled = slots::fill({.spec = world.spec,
                                            .fields = fields,
                                            .arguments = Json::Value(Json::objectValue),
                                            .context = world.context,
                                            .now = now,
                                            .text = text,
                                            .modules = world.modules,
                                            .answering = false});
  const auto resolved = call_time::resolve({.text = utterance, .lang = "es", .now = now});
  REQUIRE(resolved.has_value());
  MESSAGE("gliner turn: title='" << filled.arguments["title"].asString() << "' starts_at='"
                                 << filled.arguments["starts_at"].asString() << "' missing=" << filled.missing.size());
  CHECK(filled.missing.empty());
  const std::optional<std::int64_t> at = iso_time::parse(filled.arguments["starts_at"].asString());
  REQUIRE(at.has_value());
  CHECK(*at == resolved->fireAt);
  CHECK(*at == utc(8, 17));
  CHECK(filled.arguments["title"].isString());
  CHECK_FALSE(filled.arguments["title"].asString().empty());
  CHECK(utterance.find(filled.arguments["title"].asString()) != std::string::npos);
}

TEST_CASE("the warm resident set the GLiNER model adds is measured in service")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no GLiNER pilot bundle at ARGUS_GLINER_BUNDLE and ARGUS_GLINER_PIN; the warm resident reading did not run");
    return;
  }
  const turn::BundleLoader bundle({.dir = pilot->dir, .pin = pilot->pin, .kind = turn::BundleKind::Extractor});
  REQUIRE(bundle.valid());
  const char* arena = std::getenv("ARGUS_GLINER_RSS_CPU_ARENA");
  const char* pattern = std::getenv("ARGUS_GLINER_RSS_MEM_PATTERN");
  turn::OnnxOptions options;
  options.cpuArena = arena != nullptr && std::string(arena) != "0";
  options.memPattern = pattern != nullptr && std::string(pattern) != "0";
  const std::int64_t before = turn::residentBytes();
  const std::unique_ptr<turn::GlinerModel> model = turn::openGlinerModel(bundle, options);
  REQUIRE(model->status() == turn::EngineStatus::Ready);
  const double loadMb = static_cast<double>(model->warmRssBytes()) / (1024.0 * 1024.0);
  const std::vector<turn::GlinerSpan> spans = model->spans(
      {.text = "por favor crea un recordatorio para misa los domingos a las once de la mañana", .lang = "es", .type = "event"});
  const double warmMb = static_cast<double>(turn::residentBytes() - before) / (1024.0 * 1024.0);
  MESSAGE("gliner warm rss: cpu_arena=" << options.cpuArena << " mem_pattern=" << options.memPattern << " session load delta "
                                        << loadMb << " MB, process warm delta " << warmMb << " MB");
  CHECK(model->warmRssBytes() > 0);
  CHECK(fieldSpans(spans, "title").size() == 1);
}
