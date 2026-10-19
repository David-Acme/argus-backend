#include <doctest/doctest.h>

#include <feature/llm/services/turn/laya-sequence.hxx>
#include <feature/llm/services/turn/sp-tokenizer.hxx>

#include <json/reader.h>
#include <json/value.h>
#include <nlohmann/json.hpp>

#include <text/sha256.hxx>
#include <text/text-norm.hxx>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "support/require-value.hxx"

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

struct Parity
{
  nlohmann::ordered_json root;
  fs::path tokenizer;
};

std::optional<std::string> fileSha256(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return std::nullopt;
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return argus::hash::sha256Hex(bytes);
}

std::optional<Parity> parityFixture()
{
  const char* chosen = std::getenv("ARGUS_LAYA_BUNDLE");
  const fs::path bundle = chosen != nullptr ? fs::path(chosen) : fs::path(ARGUS_TEST_LAYA_BUNDLE);
  const fs::path tokenizer = bundle / "tokenizer" / "tokenizer.json";
  std::ifstream in(ARGUS_TEST_LAYA_SEQUENCES);
  REQUIRE(in.is_open());
  Parity parity{.root = nlohmann::ordered_json::parse(in), .tokenizer = tokenizer};
  const std::optional<std::string> digest = fileSha256(tokenizer);
  if (!digest) {
    MESSAGE("no Laya bundle tokenizer at " << tokenizer.string() << "; the Laya parity did not run");
    return std::nullopt;
  }
  if (*digest != parity.root.at("tokenizerSha256").get<std::string>()) {
    MESSAGE("the tokenizer at " << tokenizer.string()
                                << " is not the one the parity fixture was frozen from; regenerate it with tests/eval/laya-parity.py");
    return std::nullopt;
  }
  return parity;
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

TEST_CASE("python's json.dumps is what a structured criterion renders as")
{
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse(R"({"b": 1, "a": [true, null, 2.5]})")) == R"({"b": 1, "a": [true, null, 2.5]})");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse(R"(["ñ", "\"q\"", "\u0001"])")) == R"(["ñ", "\"q\"", "\u0001"])");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse("1.0")) == "1.0");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse("1e-05")) == "1e-05");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse("0.0001")) == "0.0001");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse("1e16")) == "1e+16");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse("1e15")) == "1000000000000000.0");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse("-2.25")) == "-2.25");
  CHECK(turn::layaPythonJson(nlohmann::ordered_json::parse("{}")) == "{}");
}

TEST_CASE("a question laya would refuse is refused, never rendered another way")
{
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "choice", "instructions": "x", "criteria": []})")));
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "choice", "instructions": "x", "criteria": ["a", "a"]})")));
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "choice", "instructions": " ", "criteria": ["a"]})")));
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "choice", "instructions": "x", "criteria": ["a"], "labels": {}})")));
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "score", "instructions": "x", "criteria": ["a", null]})")));
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "noul", "instructions": "x", "criteria": {"maybe": "y"}})")));
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "noul", "instructions": "x", "labels": {"false": "s", "true": " s "}})")));
  CHECK_FALSE(turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "rank", "instructions": "x", "criteria": ["a"]})")));
  const std::optional<turn::LayaQuestion> noul =
      turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "noul", "instructions": "x", "criteria": {"TRUE": "y"}})"));
  REQUIRE(noul.has_value());
  CHECK(requireValue(noul).options == std::vector<std::string>{"false: no, the statement does not hold", "true: y"});
  const std::optional<turn::LayaQuestion> choice =
      turn::layaQuestionFromJson(nlohmann::ordered_json::parse(R"({"type": "choice", "instructions": "x", "criteria": {"z": null, "a": "d"}})"));
  REQUIRE(choice.has_value());
  CHECK(requireValue(choice).labels == std::vector<std::string>{"z", "a"});
  CHECK(requireValue(choice).options == std::vector<std::string>{"z", "a: d"});
}

TEST_CASE("the Laya sequences are the training renderer's, id for id and marker for marker")
{
  const std::optional<Parity> parity = parityFixture();
  if (!parity)
    return;
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(requireValue(parity).tokenizer));
  const nlohmann::ordered_json& root = requireValue(parity).root;
  std::size_t tokenMismatches = 0;
  for (const nlohmann::ordered_json& state : root.at("states")) {
    const auto expected = state.at("tokens").get<std::vector<std::int32_t>>();
    std::string text = state.at("text").get<std::string>();
    for (std::size_t at = text.find(tokenizer.maskToken()); at != std::string::npos; at = text.find(tokenizer.maskToken(), at + 1))
      text.replace(at, tokenizer.maskToken().size(), " ");
    if (tokenizer.encode(text, false) != expected) {
      ++tokenMismatches;
      MESSAGE("state '" << state.at("text").get<std::string>().substr(0, 40) << "': the token ids differ");
    }
  }
  std::size_t sequences = 0;
  std::size_t sequenceMismatches = 0;
  const nlohmann::ordered_json& questions = root.at("questions");
  for (const nlohmann::ordered_json& item : root.at("cases")) {
    const auto qid = item.at("question").get<std::string>();
    const std::optional<turn::LayaQuestion> question = turn::layaQuestionFromJson(questions.at(qid));
    REQUIRE_MESSAGE(question.has_value(), qid);
    const turn::LayaSequence built = turn::layaBuildSequence({.tokenizer = tokenizer,
                                                              .state = item.at("state").get<std::string>(),
                                                              .maxLen = item.at("maxLen").get<int>(),
                                                              .headMaxLen = item.at("headMaxLen").get<int>()},
                                                             requireValue(question));
    const auto expectedIds = item.at("ids").get<std::vector<std::int32_t>>();
    const auto expectedMarkers = item.at("markers").get<std::vector<std::size_t>>();
    ++sequences;
    if (built.ids == expectedIds && built.markers == expectedMarkers)
      continue;
    ++sequenceMismatches;
    std::size_t at = 0;
    while (at < built.ids.size() && at < expectedIds.size() && built.ids[at] == expectedIds[at])
      ++at;
    MESSAGE(qid << " max_len=" << item.at("maxLen").get<int>() << " head_max_len=" << item.at("headMaxLen").get<int>() << ": "
                << built.ids.size() << " ids against " << expectedIds.size() << ", first difference at " << at);
    MESSAGE("  built   : " << around(built.ids, at) << "| markers " << first(built.markers));
    MESSAGE("  expected: " << around(expectedIds, at) << "| markers " << first(expectedMarkers));
  }
  MESSAGE("laya sequence parity: " << sequences << " sequences, " << sequenceMismatches << " differing, " << tokenMismatches
                                   << " state tokenizations differing");
  CHECK(sequences == root.at("cases").size());
  CHECK(tokenMismatches == 0);
  CHECK(sequenceMismatches == 0);
}

TEST_CASE("the shipped question set is the one the parity fixture renders")
{
  const std::optional<Parity> parity = parityFixture();
  if (!parity)
    return;
  const fs::path questions = requireValue(parity).tokenizer.parent_path().parent_path() / "questions.json";
  std::ifstream in(questions);
  REQUIRE(in.is_open());
  const nlohmann::ordered_json shipped = nlohmann::ordered_json::parse(in, nullptr, false);
  REQUIRE_FALSE(shipped.is_discarded());
  REQUIRE(shipped.is_object());
  const nlohmann::ordered_json& rendered = requireValue(parity).root.at("questions");
  std::size_t checked = 0;
  for (const auto& [id, question] : shipped.items()) {
    REQUIRE_MESSAGE(rendered.contains(id), id);
    CHECK(question == rendered.at(id));
    ++checked;
  }
  CHECK(checked > 0);
}

TEST_CASE("the pipeline's NFC puts a decomposed spelling on the same ids as its composed twin")
{
  const std::optional<Parity> parity = parityFixture();
  if (!parity)
    return;
  std::ifstream in(ARGUS_TEST_NFC_PAIRS);
  REQUIRE(in.is_open());
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  REQUIRE(Json::parseFromStream(builder, in, &root, &errors));
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(requireValue(parity).tokenizer));
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
