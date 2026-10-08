#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/laya-sequence.hxx>
#include <feature/llm/services/turn/sp-tokenizer.hxx>

#include <json/reader.h>
#include <json/value.h>
#include <nlohmann/json.hpp>

#include <ranges>
#include <text/text-norm.hxx>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

fs::path scratch()
{
  const fs::path root = fs::temp_directory_path() / "argus-sp-tokenizer";
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

void write(const fs::path& path, const std::string& text)
{
  std::ofstream out(path, std::ios::binary);
  out << text;
}

const char* kBpe = R"({
 "added_tokens": [
  {"id": 0, "content": "<pad>", "special": true},
  {"id": 1, "content": "<eos>", "special": true},
  {"id": 2, "content": "<bos>", "special": true},
  {"id": 3, "content": "<unk>", "special": true},
  {"id": 4, "content": "<mask>", "special": true}
 ],
 "normalizer": {"type": "Replace", "pattern": {"String": " "}, "content": "▁"},
 "pre_tokenizer": {"type": "Metaspace", "replacement": "▁", "prepend_scheme": "always", "split": true},
 "model": {
  "type": "BPE", "byte_fallback": true, "ignore_merges": false, "fuse_unk": true, "unk_token": "<unk>",
  "vocab": {"<pad>": 0, "<eos>": 1, "<bos>": 2, "<unk>": 3, "<mask>": 4, "▁": 5, "h": 6, "o": 7,
            "l": 8, "a": 9, "▁h": 10, "▁ho": 11, "▁hol": 12, "▁hola": 13,
            "<0xC3>": 14, "<0xA9>": 15},
  "merges": [["▁", "h"], ["▁h", "o"], ["▁ho", "l"], ["▁hol", "a"]]
 }
})";

const char* kUnigram = R"({
 "added_tokens": [
  {"id": 0, "content": "<pad>", "special": true},
  {"id": 1, "content": "<eos>", "special": true},
  {"id": 2, "content": "<s>", "special": true},
  {"id": 3, "content": "</s>", "special": true},
  {"id": 4, "content": "<mask>", "special": true}
 ],
 "normalizer": {"type": "Sequence", "normalizers": [
   {"type": "Replace", "pattern": {"Regex": "\\s{2,}|[\\n\\r\\t]"}, "content": " "},
   {"type": "NFC"},
   {"type": "Strip", "strip_left": false, "strip_right": true}]},
 "pre_tokenizer": {"type": "Sequence", "pretokenizers": [
   {"type": "Metaspace", "replacement": "▁", "prepend_scheme": "always", "split": true}]},
 "model": {
  "type": "Unigram", "unk_id": 0, "byte_fallback": false,
  "vocab": [["<pad>", 0.0], ["<eos>", 0.0], ["<s>", 0.0], ["</s>", 0.0], ["<mask>", 0.0],
            ["▁", -3.0], ["h", -4.0], ["o", -4.0], ["l", -4.0], ["a", -4.0],
            ["▁ho", -2.0], ["▁hola", -1.0]]
 }
})";

const char* kByteLevelS = R"({
 "added_tokens": [
  {"id": 0, "content": "<pad>", "special": true},
  {"id": 1, "content": "<eos>", "special": true},
  {"id": 2, "content": "<bos>", "special": true},
  {"id": 3, "content": "<unk>", "special": true},
  {"id": 4, "content": "<mask>", "special": true},
  {"id": 204, "content": "<s>", "special": false},
  {"id": 213, "content": "</s>", "special": false}
 ],
 "model": {"type": "Unigram", "unk_id": 3, "vocab": [["<pad>", 0.0], ["<eos>", 0.0], ["<bos>", 0.0], ["<unk>", 0.0], ["<mask>", 0.0]]}
})";

struct Fixture
{
  std::vector<std::string> texts;
  std::vector<std::vector<std::int32_t>> tokens;
};

bool loadFixture(const fs::path& path, Fixture& fixture)
{
  std::ifstream in(path);
  if (!in.is_open())
    return false;
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  if (!Json::parseFromStream(builder, in, &root, &errors))
    return false;
  for (const Json::Value& item : root["cases"]) {
    fixture.texts.push_back(item["text"].asString());
    std::vector<std::int32_t> ids;
    for (const Json::Value& token : item["tokens"])
      ids.push_back(token.asInt());
    fixture.tokens.push_back(std::move(ids));
  }
  return true;
}

template <typename Range>
std::string first(const Range& values)
{
  std::string out;
  std::size_t taken = 0;
  for (const auto& value : values) {
    if (taken++ == 8)
      break;
    out += std::to_string(value) + " ";
  }
  return out;
}

template <typename Range>
std::string around(const Range& values, std::size_t at)
{
  std::string out;
  const std::size_t from = at > 3 ? at - 3 : 0;
  std::size_t index = 0;
  for (const auto& value : values) {
    if (index >= from && index < from + 8)
      out += std::to_string(value) + " ";
    ++index;
  }
  return out;
}

template <typename Range>
std::string last(const Range& values)
{
  std::string out;
  std::size_t index = 0;
  const std::size_t size = std::ranges::distance(values);
  for (const auto& value : values) {
    if (index + 8 >= size)
      out += std::to_string(value) + " ";
    ++index;
  }
  return out;
}

turn::LayaQuestion questionOf(const nlohmann::ordered_json& definition){
  turn::LayaQuestion question;
  const std::string type = definition.value("type", "");
  question.kind = type == "score"    ? turn::QuestionKind::Score
                  : type == "noul" ? turn::QuestionKind::Noul
                                   : turn::QuestionKind::Choice;
  question.instructions = definition.value("instructions", "");
  if (definition.contains("criteria")) {
    const nlohmann::ordered_json& criteria = definition["criteria"];
    if (criteria.is_object()) {
      for (auto entry = criteria.begin(); entry != criteria.end(); ++entry) {
        const nlohmann::ordered_json& value = entry.value();
        question.criteria.push_back({entry.key(), value.is_null() ? std::string() : value.is_string() ? value.get<std::string>() : value.dump()});
      }
    }
    else if (criteria.is_array()) {
      for (const nlohmann::ordered_json& value : criteria)
        question.criteria.push_back({value.is_string() ? value.get<std::string>() : value.dump(), ""});
    }
  }
  if (definition.contains("labels")) {
    question.falseLabel = definition["labels"].value("false", "false");
    question.trueLabel = definition["labels"].value("true", "true");
  }
  return question;
}
}

TEST_CASE("a sentencepiece BPE tokenizes with its marker, its merges and its byte fallback")
{
  const fs::path root = scratch();
  write(root / "tokenizer.json", kBpe);
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(root / "tokenizer.json"));
  CHECK(tokenizer.model() == turn::SpModel::Bpe);
  CHECK(tokenizer.encode("hola", false) == std::vector<std::int32_t>{13});
  CHECK(tokenizer.encode("hola", true) == std::vector<std::int32_t>{2, 13, 1});
  CHECK(tokenizer.encode("", false).empty());
  CHECK(tokenizer.encode("café", false) != std::vector<std::int32_t>{});
  CHECK(tokenizer.encode("é", false) == std::vector<std::int32_t>{5, 14, 15});
}

TEST_CASE("a sentencepiece unigram picks the best segmentation, and strips and collapses first")
{
  const fs::path root = scratch();
  write(root / "tokenizer.json", kUnigram);
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(root / "tokenizer.json"));
  CHECK(tokenizer.model() == turn::SpModel::Unigram);
  CHECK(tokenizer.encode("hola", false) == std::vector<std::int32_t>{11});
  CHECK(tokenizer.encode("hola", true) == std::vector<std::int32_t>{2, 11, 1});
  CHECK(tokenizer.encode("hola   ", false) == std::vector<std::int32_t>{11});
  CHECK(tokenizer.encode("", false).empty());
}

TEST_CASE("a byte-level <s> never displaces the checkpoint's own <bos>")
{
  const fs::path root = scratch();
  write(root / "tokenizer.json", kByteLevelS);
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(root / "tokenizer.json"));
  CHECK(tokenizer.clsId() == 2);
  CHECK(tokenizer.sepId() == 1);
  CHECK(tokenizer.maskId() == 4);
}

TEST_CASE("a tokenizer file that is not a sentencepiece model is refused")
{
  const fs::path root = scratch();
  write(root / "tokenizer.json", R"({"model": {"type": "WordPiece", "vocab": {"a": 0}}})");
  turn::SpTokenizer tokenizer;
  CHECK_FALSE(tokenizer.load(root / "tokenizer.json"));
  CHECK_FALSE(tokenizer.loaded());
  CHECK(tokenizer.encode("hola", false).empty());
}

TEST_CASE("the frozen Laya fixture's token ids and sequences are reproduced row for row")
{
  const char* checkpoint = std::getenv("ARGUS_LAYA_CHECKPOINT");
  const fs::path tokenizerPath = checkpoint != nullptr ? fs::path(checkpoint) / "tokenizer" / "tokenizer.json" : fs::path();
  if (checkpoint == nullptr || !fs::exists(tokenizerPath)) {
    MESSAGE("no Laya checkpoint at ARGUS_LAYA_CHECKPOINT; the Laya parity did not run");
    return;
  }
  Fixture fixture;
  REQUIRE(loadFixture(ARGUS_TEST_LAYA_FIXTURE, fixture));
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(tokenizerPath));
  nlohmann::ordered_json questions =
      nlohmann::ordered_json::parse(std::ifstream(fs::path(checkpoint) / (std::getenv("ARGUS_LAYA_QUESTIONS") != nullptr ? std::getenv("ARGUS_LAYA_QUESTIONS")
                                                                                                                      : "questions.json")));
  nlohmann::ordered_json config = nlohmann::ordered_json::parse(std::ifstream(fs::path(checkpoint) / "rl_agent_config.json"));
  const int maxLen = config.value("max_len", 512);
  const int headMaxLen = config.value("head_max_len", 192);
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  {
    std::ifstream in(ARGUS_TEST_LAYA_FIXTURE);
    REQUIRE(Json::parseFromStream(builder, in, &root, &errors));
  }
  std::size_t tokenMismatches = 0;
  std::size_t sequenceMismatches = 0;
  for (std::size_t row = 0; row < fixture.texts.size(); ++row) {
    if (tokenizer.encode(fixture.texts[row], false) != fixture.tokens[row]) {
      ++tokenMismatches;
      MESSAGE("row " << row << ": the token ids differ");
    }
    const Json::Value& sequences = root["cases"][static_cast<Json::ArrayIndex>(row)]["sequences"];
    for (const std::string& qid : sequences.getMemberNames()) {
      const turn::LayaQuestion question = questionOf(questions.at(qid));
      const turn::LayaSequence built =
          turn::layaBuildSequence({.tokenizer = tokenizer, .state = fixture.texts[row], .maxLen = maxLen, .headMaxLen = headMaxLen}, question);
      const Json::Value& expected = sequences[qid];
      std::vector<std::int32_t> expectedIds;
      for (const Json::Value& id : expected["ids"])
        expectedIds.push_back(id.asInt());
      std::vector<std::size_t> expectedMarkers;
      for (const Json::Value& marker : expected["markers"])
        expectedMarkers.push_back(static_cast<std::size_t>(marker.asUInt()));
      if (built.ids != expectedIds || built.markers != expectedMarkers) {
        ++sequenceMismatches;
        std::size_t at = 0;
        while (at < built.ids.size() && at < expectedIds.size() && built.ids[at] == expectedIds[at])
          ++at;
        MESSAGE("row " << row << " question " << qid << ": " << built.ids.size() << " ids against " << expectedIds.size()
                        << ", " << built.markers.size() << " markers against " << expectedMarkers.size() << ", first difference at "
                        << at << " built=" << (at < built.ids.size() ? built.ids[at] : -1)
                        << " expected=" << (at < expectedIds.size() ? expectedIds[at] : -1));
        MESSAGE("  built   : " << first(built.ids) << " | " << around(built.ids, at) << " | " << last(built.ids));
        MESSAGE("  expected: " << first(expectedIds) << " | " << around(expectedIds, at) << " | " << last(expectedIds));
        MESSAGE("  markers : " << first(built.markers) << " | " << first(expectedMarkers));
      }
    }
  }
  CHECK(tokenMismatches == 0);
  CHECK(sequenceMismatches == 0);
}

TEST_CASE("the pipeline's NFC puts a decomposed spelling on the same ids as its composed twin")
{
  const char* checkpoint = std::getenv("ARGUS_LAYA_CHECKPOINT");
  const fs::path tokenizerPath = checkpoint != nullptr ? fs::path(checkpoint) / "tokenizer" / "tokenizer.json" : fs::path();
  if (checkpoint == nullptr || !fs::exists(tokenizerPath)) {
    MESSAGE("no Laya checkpoint at ARGUS_LAYA_CHECKPOINT; the NFC pair parity did not run");
    return;
  }
  std::ifstream in(ARGUS_TEST_NFC_PAIRS);
  REQUIRE(in.is_open());
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  REQUIRE(Json::parseFromStream(builder, in, &root, &errors));
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(tokenizerPath));
  std::size_t mismatches = 0;
  for (const Json::Value& pair : root["pairs"]) {
    std::vector<std::int32_t> composed;
    for (const Json::Value& id : pair["idsNfc"])
      composed.push_back(id.asInt());
    if (tokenizer.encode(text_norm::nfc(pair["nfc"].asString()), false) != composed)
      ++mismatches;
    if (tokenizer.encode(text_norm::nfc(pair["nfd"].asString()), false) != composed)
      ++mismatches;
  }
  CHECK(mismatches == 0);
}
