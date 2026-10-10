#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/bundle-loader.hxx>

#include <text/sha256.hxx>

#include <json/reader.h>
#include <json/value.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <stdexcept>

#include "support/require-value.hxx"

namespace
{
namespace fs = std::filesystem;

struct Built
{
  fs::path dir;
  std::string pin;
};

void write(const fs::path& path, const std::string& bytes)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << bytes;
}

std::string readText(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> collect(const fs::path& dir)
{
  std::vector<std::string> names;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir))
    if (entry.is_regular_file())
      names.push_back(entry.path().lexically_relative(dir).generic_string());
  std::ranges::sort(names);
  return names;
}

std::string listing(const fs::path& dir)
{
  std::string out;
  for (const std::string& name : collect(dir))
    if (name != "sha256")
      out += argus::hash::sha256Hex(readText(dir / name)) + "  " + name + "\n";
  return out;
}

fs::path scratch(const std::string& name)
{
  const fs::path root = fs::temp_directory_path() / ("argus-bundle-" + name);
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

void seal(const fs::path& dir)
{
  write(dir / "sha256", listing(dir));
}

Json::Value parseJson(const std::string& text)
{
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::istringstream stream(text);
  std::string errors;
  Json::parseFromStream(builder, stream, &root, &errors);
  return root;
}

Built built(const std::string& name)
{
  const fs::path root = scratch(name);
  write(root / "model.onnx", "weights");
  write(root / "tokenizer" / "tokenizer.json", "{}");
  write(root / "labels.json", R"({"labels": ["memory_save", "camera"], "skew": 0.1})");
  write(root / "decision.json",
        R"({"act": 0.84, "ask": 0.84, "margin": 0.1, "now": 0.7, "guardMemory": false,
            "head_max_len": 256, "temperature": [3.0, 1.0, 5.0],
            "calibration": {"confidence": {"type": "platt", "scale": 2.0, "shift": -0.5}},
            "source": {"fitOn": "calibration"}})");
  write(root / "max_len", "512\n");
  write(root / "model-card.md", "# card\n");
  write(root / "manifest.json", R"({"calibration": {"fitOn": "calibration"}, "labels": 2})");
  seal(root);
  return {.dir = root, .pin = argus::hash::sha256Hex(readText(root / "sha256"))};
}
}

TEST_CASE("a complete bundle loads its pin, its labels and its policy")
{
  const Built bundle = built("valid");
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = bundle.pin});
  CHECK(loader.valid());
  CHECK(loader.error().empty());
  CHECK(loader.maxLen() == 512);
  CHECK(loader.labels().size() == 2);
  CHECK(std::ranges::find(loader.labels(), "memory_save") != loader.labels().end());
  CHECK(std::ranges::find(loader.labels(), "camera") != loader.labels().end());
  CHECK(loader.policy().present);
  CHECK(loader.policy().act == doctest::Approx(0.84));
  CHECK(loader.policy().now == doctest::Approx(0.7));
  CHECK(loader.fitSplit() == "calibration");
  CHECK(loader.confidenceCalibration().type == "platt");
  CHECK(loader.confidenceCalibration().apply(0.5) ==
        doctest::Approx(turn::sigmoidOf(2.0 * turn::logitOf(0.5) - 0.5)));
  CHECK(loader.tokenizerJson() == bundle.dir / "tokenizer" / "tokenizer.json");
}

TEST_CASE("a decider bundle reads the head_max_len and temperature it declares")
{
  const Built bundle = built("decode-ok");
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = bundle.pin});
  CHECK(loader.valid());
  CHECK(loader.error().empty());
  CHECK(loader.decode().headMaxLen == 256);
  CHECK(loader.decode().temperature[0] == doctest::Approx(3.0));
  CHECK(loader.decode().temperature[1] == doctest::Approx(1.0));
  CHECK(loader.decode().temperature[2] == doctest::Approx(5.0));
}

TEST_CASE("a decider bundle that declares neither head_max_len nor temperature is refused")
{
  const Built bundle = built("decode-absent");
  write(bundle.dir / "decision.json",
        R"({"act": 0.84, "ask": 0.84, "margin": 0.1, "now": 0.7, "guardMemory": false,
            "calibration": {"confidence": {"type": "platt", "scale": 2.0, "shift": -0.5}},
            "source": {"fitOn": "calibration"}})");
  seal(bundle.dir);
  const turn::BundleLoader loader({.dir = bundle.dir,
                                    .pin = argus::hash::sha256Hex(readText(bundle.dir / "sha256"))});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("declares neither head_max_len nor temperature") != std::string::npos);
}

TEST_CASE("a decider bundle that declares only one of head_max_len and temperature is refused")
{
  const Built bundle = built("decode-partial");
  write(bundle.dir / "decision.json",
        R"({"act": 0.84, "ask": 0.84, "margin": 0.1, "now": 0.7, "guardMemory": false,
            "head_max_len": 256,
            "calibration": {"confidence": {"type": "platt", "scale": 2.0, "shift": -0.5}},
            "source": {"fitOn": "calibration"}})");
  seal(bundle.dir);
  const turn::BundleLoader loader({.dir = bundle.dir,
                                    .pin = argus::hash::sha256Hex(readText(bundle.dir / "sha256"))});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("declares only one of them") != std::string::npos);
}

TEST_CASE("the pin is the hash of the sha256 file, and nothing else")
{
  const Built bundle = built("pin");
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = std::string(64, 'a')});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("pin") != std::string::npos);
}

TEST_CASE("a name the sha256 file lists but the bundle lacks is refused")
{
  const Built bundle = built("absent");
  write(bundle.dir / "sha256", listing(bundle.dir) + argus::hash::sha256Hex("ghost") + "  tokenizer/ghost.json\n");
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = argus::hash::sha256Hex(readText(bundle.dir / "sha256"))});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("not in the bundle") != std::string::npos);
}

TEST_CASE("a file the bundle holds but the sha256 file does not name is refused")
{
  const Built bundle = built("extra");
  write(bundle.dir / "extra.txt", "smuggled");
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = bundle.pin});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("does not list") != std::string::npos);
}

TEST_CASE("a file that does not match its line is refused")
{
  const Built bundle = built("changed");
  write(bundle.dir / "labels.json", R"({"labels": ["camera"]})");
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = bundle.pin});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("does not match") != std::string::npos);
}

TEST_CASE("a bundle missing a layout file is refused")
{
  const fs::path root = scratch("incomplete");
  write(root / "model.onnx", "weights");
  write(root / "tokenizer" / "tokenizer.json", "{}");
  write(root / "labels.json", R"({"labels": ["camera"]})");
  write(root / "decision.json", R"({"source": {"fitOn": "calibration"}})");
  write(root / "model-card.md", "# card\n");
  write(root / "manifest.json", "{}");
  seal(root);
  const turn::BundleLoader loader({.dir = root, .pin = argus::hash::sha256Hex(readText(root / "sha256"))});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("missing max_len") != std::string::npos);
}

TEST_CASE("a listing that names a file the bundle lacks is refused before the layout is read")
{
  const Built bundle = built("vanished");
  fs::remove(bundle.dir / "max_len");
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = bundle.pin});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("not in the bundle") != std::string::npos);
}

TEST_CASE("a bundle that is not there is refused by name")
{
  const turn::BundleLoader loader({.dir = scratch("gone") / "nope", .pin = std::string(64, 'b')});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("nope") != std::string::npos);
}

TEST_CASE("an isotonic calibration is piecewise linear, as the harness fits it")
{
  const Json::Value node = parseJson(R"({"type": "isotonic", "lower": [0.0, 0.5, 0.8],
                                         "upper": [0.4, 0.7, 1.0], "value": [0.1, 0.6, 0.9]})");
  const std::optional<turn::CalibrationModel> model = turn::calibrationFromJson(node);
  REQUIRE(model.has_value());
  CHECK(requireValue(model).apply(0.2) == doctest::Approx(0.1));
  CHECK(requireValue(model).apply(0.6) == doctest::Approx(0.6));
  CHECK(requireValue(model).apply(0.95) == doctest::Approx(0.9));
  CHECK(requireValue(model).apply(0.75) == doctest::Approx(0.75));
}

TEST_CASE("an unknown calibration kind is refused rather than applied as identity")
{
  const Json::Value node = parseJson(R"({"type": "beta"})");
  CHECK_FALSE(turn::calibrationFromJson(node).has_value());
}

TEST_CASE("a decider bundle's label array keeps its declaration order")
{
  const Built bundle = built("labels");
  write(bundle.dir / "labels.json", R"({"labels": ["zebra", "alpha", "middle"]})");
  seal(bundle.dir);
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = argus::hash::sha256Hex(readText(bundle.dir / "sha256"))});
  CHECK(loader.valid());
  CHECK(loader.labels() == std::vector<std::string>{"zebra", "alpha", "middle"});
}

TEST_CASE("an extractor bundle's logit order is the type-keyed map, each type a field array")
{
  const Built bundle = built("extractor");
  write(bundle.dir / "labels.json",
        R"({"model": "gliner", "logitOrder": "perType",
            "types": {"event": ["title", "people"], "project": ["name"]}})");
  write(bundle.dir / "decision.json", R"({"pairThreshold": 0.3, "maxSpanWords": 11, "source": {"fitOn": "calibration"}})");
  seal(bundle.dir);
  const turn::BundleLoader loader(
      {.dir = bundle.dir, .pin = argus::hash::sha256Hex(readText(bundle.dir / "sha256")), .kind = turn::BundleKind::Extractor});
  CHECK(loader.valid());
  REQUIRE(loader.typeFields().size() == 2);
  CHECK(loader.typeFields().at("event") == std::vector<std::string>{"title", "people"});
  CHECK(loader.typeFields().at("project") == std::vector<std::string>{"name"});
  CHECK(loader.labels().empty());
  CHECK(loader.thresholds().present);
  CHECK(loader.thresholds().threshold == doctest::Approx(0.3));
  CHECK(loader.thresholds().maxSpanWidth == 11);
}

TEST_CASE("an extractor bundle that carries a label array instead of the type map is refused")
{
  const Built bundle = built("extractor-array");
  const turn::BundleLoader loader(
      {.dir = bundle.dir, .pin = bundle.pin, .kind = turn::BundleKind::Extractor});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("type-keyed object form") != std::string::npos);
}

TEST_CASE("a decider bundle that carries the type-keyed map instead of a label array is refused")
{
  const Built bundle = built("decider-object");
  write(bundle.dir / "labels.json", R"({"model": "gliner", "types": {"event": ["title"]}})");
  seal(bundle.dir);
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = argus::hash::sha256Hex(readText(bundle.dir / "sha256"))});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("label array form") != std::string::npos);
}

TEST_CASE("a decider bundle that carries an object of labels instead of an array is refused")
{
  const Built bundle = built("decider-descriptions");
  write(bundle.dir / "labels.json", R"({"labels": {"memory_save": "a fact", "camera": "a camera"}})");
  seal(bundle.dir);
  const turn::BundleLoader loader({.dir = bundle.dir, .pin = argus::hash::sha256Hex(readText(bundle.dir / "sha256"))});
  CHECK_FALSE(loader.valid());
  CHECK(loader.error().find("label array form") != std::string::npos);
}
