#include "gliner-extractor.hxx"

#include "gliner-decoder.hxx"
#include "sp-tokenizer.hxx"

#include <text/text-norm.hxx>

#include <trantor/utils/Logger.h>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace turn
{

namespace
{
constexpr int kWordAxis = 128;
constexpr int kQueryAxis = 3;
constexpr double kDefaultPairThreshold = 0.1;
constexpr int kDefaultMaxSpanWords = 16;
constexpr std::string_view kStructureSeparator = "[SEP_TEXT]";
constexpr std::string_view kTypeMarker = "[P]";
constexpr std::string_view kFieldMarker = "[C]";

struct ToolType
{
  std::string_view tool;
  std::string_view type;
};

constexpr std::array<ToolType, 7> kToolTypes{{
    {.tool = "calendar.create_event", .type = "event"},
    {.tool = "calendar.cancel_event", .type = "event"},
    {.tool = "task.create", .type = "task"},
    {.tool = "task.complete", .type = "task"},
    {.tool = "project.create", .type = "project"},
    {.tool = "memory.remind", .type = "reminder"},
    {.tool = "memory.remember", .type = "fact"},
}};

constexpr std::array<char, 3> kTerminators{'.', '!', '?'};

struct Codepoint
{
  std::uint32_t value{0};
  std::size_t begin{0};
  std::size_t end{0};
};

struct Slice
{
  std::size_t begin{0};
  std::size_t end{0};
};

struct WordSpan
{
  std::size_t begin{0};
  std::size_t end{0};
  std::size_t byteBegin{0};
  std::size_t byteEnd{0};
};

std::vector<Codepoint> codepointsOf(std::string_view text)
{
  std::vector<Codepoint> out;
  out.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t size = 1;
    if ((lead & 0xE0U) == 0xC0U)
      size = 2;
    else if ((lead & 0xF0U) == 0xE0U)
      size = 3;
    else if ((lead & 0xF8U) == 0xF0U)
      size = 4;
    if (at + size > text.size())
      size = 1;
    utf8proc_int32_t value = 0;
    const utf8proc_ssize_t read =
        utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data() + at), static_cast<utf8proc_ssize_t>(size), &value);
    if (read <= 0) {
      value = static_cast<utf8proc_int32_t>(lead);
      size = 1;
    }
    out.push_back({.value = static_cast<std::uint32_t>(value), .begin = at, .end = at + size});
    at += size;
  }
  return out;
}

std::uint32_t asciiLower(std::uint32_t value)
{
  return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

bool asciiAlpha(std::uint32_t value)
{
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool asciiDigit(std::uint32_t value)
{
  return value >= '0' && value <= '9';
}

bool spaceValue(std::uint32_t value)
{
  if (value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v')
    return true;
  if (value >= 0x1CU && value <= 0x1FU)
    return true;
  if (value == 0x85U)
    return true;
  const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(value));
  return category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP;
}

bool wordValue(std::uint32_t value)
{
  if (value == '_' || asciiAlpha(value) || asciiDigit(value))
    return true;
  const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(value));
  return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL || category == UTF8PROC_CATEGORY_LT ||
         category == UTF8PROC_CATEGORY_LM || category == UTF8PROC_CATEGORY_LO || category == UTF8PROC_CATEGORY_ND ||
         category == UTF8PROC_CATEGORY_NL || category == UTF8PROC_CATEGORY_NO;
}

bool localValue(std::uint32_t value)
{
  return asciiAlpha(value) || asciiDigit(value) || value == '.' || value == '_' || value == '%' || value == '+' ||
         value == '-';
}

bool domainValue(std::uint32_t value)
{
  return asciiAlpha(value) || asciiDigit(value) || value == '.' || value == '-';
}

bool handleValue(std::uint32_t value)
{
  return asciiAlpha(value) || asciiDigit(value) || value == '_';
}

std::vector<WordSpan> splitSpans(std::string_view text)
{
  const std::vector<Codepoint> points = codepointsOf(text);
  const auto runEnd = [&points](std::size_t from, const auto& accepts) {
    std::size_t end = from;
    while (end < points.size() && accepts(points[end].value))
      ++end;
    return end;
  };
  const auto startsWithFold = [&points](std::size_t at, std::string_view prefix) {
    if (at + prefix.size() > points.size())
      return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (asciiLower(points[at + index].value) != static_cast<unsigned char>(prefix[index]))
        return false;
    }
    return true;
  };
  const auto matchAt = [&points, &runEnd, &startsWithFold](std::size_t at) -> std::optional<Slice> {
    if (startsWithFold(at, "https://") || startsWithFold(at, "http://") || startsWithFold(at, "www."))
      return Slice{.begin = at, .end = runEnd(at, [](std::uint32_t value) { return !spaceValue(value); })};
    const std::size_t local = runEnd(at, localValue);
    if (local > at && local < points.size() && points[local].value == '@') {
      const std::size_t domain = runEnd(local + 1, domainValue);
      for (std::size_t dot = domain; dot > local + 1; --dot) {
        if (points[dot - 1].value != '.')
          continue;
        const std::size_t letters = runEnd(dot, asciiAlpha);
        if (letters - dot >= 2)
          return Slice{.begin = at, .end = letters};
      }
    }
    if (points[at].value == '@') {
      const std::size_t handle = runEnd(at + 1, handleValue);
      if (handle > at + 1)
        return Slice{.begin = at, .end = handle};
    }
    if (wordValue(points[at].value)) {
      std::size_t end = runEnd(at, wordValue);
      while (end + 1 < points.size() && (points[end].value == '-' || points[end].value == '_') &&
             wordValue(points[end + 1].value))
        end = runEnd(end + 1, wordValue);
      return Slice{.begin = at, .end = end};
    }
    if (!spaceValue(points[at].value))
      return Slice{.begin = at, .end = at + 1};
    return std::nullopt;
  };
  std::vector<WordSpan> words;
  std::size_t at = 0;
  while (at < points.size()) {
    const std::optional<Slice> found = matchAt(at);
    if (!found) {
      ++at;
      continue;
    }
    words.push_back({.begin = found->begin,
                     .end = found->end,
                     .byteBegin = points[found->begin].begin,
                     .byteEnd = points[found->end - 1].end});
    at = found->end;
  }
  return words;
}

std::string lowered(std::string_view text)
{
  const std::vector<Codepoint> points = codepointsOf(text);
  std::string out;
  out.reserve(text.size());
  for (const Codepoint& point : points) {
    std::array<utf8proc_uint8_t, 4> buffer{};
    const utf8proc_ssize_t written =
        utf8proc_encode_char(utf8proc_tolower(static_cast<utf8proc_int32_t>(point.value)), buffer.data());
    if (written <= 0)
      out.append(text.substr(point.begin, point.end - point.begin));
    else
      out.append(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(written));
  }
  return out;
}

bool endsWithTerminator(std::string_view text)
{
  if (text.empty())
    return false;
  return std::ranges::find(kTerminators, text.back()) != kTerminators.end();
}

std::string terminated(std::string_view text)
{
  if (text.empty())
    return ".";
  if (endsWithTerminator(text))
    return std::string(text);
  return std::string(text) + '.';
}

std::string truncated(std::string_view text)
{
  const std::vector<WordSpan> words = splitSpans(text);
  const std::size_t count = words.size() + (endsWithTerminator(text) ? 0 : 1);
  if (count <= kWordAxis || words.size() <= static_cast<std::size_t>(kWordAxis - 2))
    return std::string(text);
  std::string kept(text.substr(0, words[kWordAxis - 2].byteEnd));
  if (!endsWithTerminator(kept))
    kept += '.';
  LOG_WARN << "argus-llm: the utterance carries " << count << " words and the GLiNER graph holds " << kWordAxis
           << "; it is cut at the " << (kWordAxis - 1) << "th word boundary";
  return kept;
}

std::vector<std::int32_t> tokenizePiece(const SpTokenizer& tokenizer, std::string_view piece)
{
  const bool plain = std::ranges::all_of(piece, [](char character) {
    return static_cast<unsigned char>(character) < 0x80U;
  });
  const std::string normalized = plain ? std::string(piece) : text_norm::nfc(piece);
  if (const std::optional<std::int32_t> added = tokenizer.addedToken(normalized))
    return {*added};
  return tokenizer.encode(normalized, false);
}

std::vector<std::string> structureTokens(std::string_view type, const std::vector<std::string>& fields)
{
  std::vector<std::string> tokens;
  tokens.reserve(fields.size() * 2 + 4);
  tokens.emplace_back("(");
  tokens.emplace_back(kTypeMarker);
  tokens.emplace_back(type);
  tokens.emplace_back("(");
  for (const std::string& field : fields) {
    tokens.emplace_back(kFieldMarker);
    tokens.emplace_back(field);
  }
  tokens.emplace_back(")");
  tokens.emplace_back(")");
  return tokens;
}

std::string trimmed(std::string_view text)
{
  const std::size_t first = text.find_first_not_of(" \t\r\n\f\v");
  if (first == std::string_view::npos)
    return {};
  const std::size_t last = text.find_last_not_of(" \t\r\n\f\v");
  return std::string(text.substr(first, last - first + 1));
}

std::int64_t wholeAt(const OnnxTensor& tensor, std::size_t index)
{
  return index < tensor.whole.size() ? tensor.whole[index] : 0;
}

float logitAt(const OnnxTensor& tensor, std::size_t index)
{
  return index < tensor.values.size() ? tensor.values[index] : 0.0F;
}

struct GraphRun
{
  const GlinerRequest& request;
  const std::vector<std::string>& fields;
  const GlinerPrompt& prompt;
  const std::vector<OnnxTensor>& outputs;
};

class GlinerBundleModel final : public GlinerModel
{
public:
  GlinerBundleModel(const BundleLoader& bundle, OnnxOptions options)
      : modelPath_(bundle.modelPath()), tokenizerPath_(bundle.tokenizerJson()), types_(bundle.typeFields()),
        options_(options),
        threshold_(bundle.thresholds().present ? bundle.thresholds().threshold : kDefaultPairThreshold),
        maxSpanWords_(bundle.thresholds().present ? bundle.thresholds().maxSpanWidth : kDefaultMaxSpanWords)
  {
  }

  [[nodiscard]] EngineStatus status() override
  {
    if (!opened_)
      open();
    return ready_ ? EngineStatus::Ready : EngineStatus::Unavailable;
  }

  [[nodiscard]] std::int64_t warmRssBytes() const override { return session_.warmRssDeltaBytes(); }

  [[nodiscard]] std::vector<GlinerSpan> spans(const GlinerRequest& request) override
  {
    if (status() != EngineStatus::Ready)
      return {};
    const auto type = types_.find(std::string(request.type));
    if (type == types_.end())
      return {};
    const std::string document = glinerDocument(request.text);
    const GlinerPrompt prompt =
        glinerPrompt({.tokenizer = tokenizer_, .document = document, .type = request.type, .fields = type->second});
    if (prompt.wordMask.size() != kWordAxis || prompt.queryMask.size() != kQueryAxis)
      return {};
    const std::vector<OnnxInput> tensors{
        {.name = "input_ids", .shape = {1, static_cast<std::int64_t>(prompt.ids.size())}, .values = prompt.ids},
        {.name = "attention_mask",
         .shape = {1, static_cast<std::int64_t>(prompt.ids.size())},
         .values = std::vector<std::int64_t>(prompt.ids.size(), 1)},
        {.name = "word_indices", .shape = {1, kWordAxis}, .values = prompt.wordIndices},
        {.name = "word_mask", .shape = {1, kWordAxis}, .values = prompt.wordMask},
        {.name = "query_indices", .shape = {1, kQueryAxis}, .values = prompt.queryIndices},
        {.name = "query_mask", .shape = {1, kQueryAxis}, .values = prompt.queryMask}};
    const std::optional<std::vector<OnnxTensor>> outputs =
        session_.run({.inputs = tensors, .outputs = {"candidate_indices", "candidate_mask", "pair_logits"}});
    if (!outputs || outputs->size() < 3)
      return {};
    if (!(*outputs)[0].integral || !(*outputs)[1].integral) {
      LOG_WARN << "argus-llm: the GLiNER graph returned a candidate tensor that is not an integer one; "
                  "the turn continues on NuExtract";
      return {};
    }
    return decode({.request = request, .fields = type->second, .prompt = prompt, .outputs = *outputs});
  }

private:
  void open()
  {
    opened_ = true;
    if (!tokenizer_.load(tokenizerPath_)) {
      LOG_WARN << "argus-llm: the GLiNER extractor could not read " << tokenizerPath_.string()
               << "; the turn continues on NuExtract";
      return;
    }
    if (!session_.open(modelPath_, options_)) {
      LOG_WARN << "argus-llm: the GLiNER extractor could not open " << modelPath_.string() << ": " << session_.error()
               << "; the turn continues on NuExtract";
      return;
    }
    ready_ = true;
    LOG_INFO << "argus-llm: the GLiNER extractor opened " << modelPath_.filename().string() << " with "
             << types_.size() << " types, pair_threshold=" << threshold_ << " max_span_words=" << maxSpanWords_;
  }

  [[nodiscard]] std::vector<GlinerSpan> decode(const GraphRun& run) const
  {
    const OnnxTensor& indices = run.outputs[0];
    const OnnxTensor& mask = run.outputs[1];
    const OnnxTensor& logits = run.outputs[2];
    if (indices.shape.size() != 4 || mask.shape.size() != 3 || logits.shape.size() != 3)
      return {};
    const auto queries = static_cast<std::size_t>(indices.shape[1]);
    const auto pool = static_cast<std::size_t>(indices.shape[2]);
    std::vector<GlinerCandidate> candidates;
    candidates.reserve(queries * pool);
    for (std::size_t query = 0; query < queries; ++query) {
      for (std::size_t slot = 0; slot < pool; ++slot) {
        const std::size_t plane = query * pool + slot;
        if (wholeAt(mask, plane) == 0)
          continue;
        candidates.push_back({.query = static_cast<std::int32_t>(query),
                              .start = static_cast<std::int32_t>(wholeAt(indices, plane * 2)),
                              .end = static_cast<std::int32_t>(wholeAt(indices, plane * 2 + 1)),
                              .logit = logitAt(logits, plane)});
      }
    }
    const std::vector<Codepoint> points = codepointsOf(run.request.text);
    std::vector<std::int32_t> starts;
    std::vector<std::int32_t> ends;
    starts.reserve(run.prompt.words.size());
    ends.reserve(run.prompt.words.size());
    for (const GlinerWord& word : run.prompt.words) {
      starts.push_back(static_cast<std::int32_t>(word.begin));
      ends.push_back(static_cast<std::int32_t>(word.end));
    }
    const std::vector<GlinerDecodedSpan> decoded = glinerDecode({.candidates = candidates,
                                                                .queryThresholds = {},
                                                                .defaultThreshold = static_cast<float>(threshold_),
                                                                .maxWidth = maxSpanWords_,
                                                                .overlap = GlinerOverlap::Flat});
    std::vector<GlinerSpan> spans;
    for (const GlinerDecodedSpan& span : decoded) {
      if (span.query < 0 || static_cast<std::size_t>(span.query) >= run.fields.size())
        continue;
      const GlinerOffsets offsets = glinerCharacterOffsets(span, starts, ends);
      if (offsets.end <= offsets.begin || static_cast<std::size_t>(offsets.begin) >= points.size())
        continue;
      const std::size_t last = std::min(static_cast<std::size_t>(offsets.end), points.size());
      if (std::cmp_less_equal(last, offsets.begin))
        continue;
      std::string surface = trimmed(run.request.text.substr(points[static_cast<std::size_t>(offsets.begin)].begin,
                                                             points[last - 1].end - points[static_cast<std::size_t>(offsets.begin)].begin));
      if (surface.empty())
        continue;
      spans.push_back({.field = run.fields[static_cast<std::size_t>(span.query)],
                       .text = std::move(surface),
                       .score = static_cast<double>(span.probability),
                       .begin = static_cast<std::size_t>(offsets.begin),
                       .end = static_cast<std::size_t>(offsets.end)});
    }
    return spans;
  }

  std::filesystem::path modelPath_;
  std::filesystem::path tokenizerPath_;
  std::map<std::string, std::vector<std::string>> types_;
  OnnxOptions options_;
  double threshold_{kDefaultPairThreshold};
  int maxSpanWords_{kDefaultMaxSpanWords};
  SpTokenizer tokenizer_;
  OnnxSession session_;
  bool opened_{false};
  bool ready_{false};
};
}

std::string glinerDocument(std::string_view text)
{
  return terminated(truncated(text));
}

std::vector<GlinerWord> glinerSplitWords(std::string_view text)
{
  std::vector<GlinerWord> words;
  for (const WordSpan& span : splitSpans(text))
    words.push_back({.begin = span.begin, .end = span.end});
  return words;
}

GlinerPrompt glinerPrompt(const GlinerPromptInput& input)
{
  const std::vector<std::string> structure = structureTokens(input.type, input.fields);
  const std::vector<WordSpan> words = splitSpans(input.document);
  std::vector<std::string> combined = structure;
  combined.reserve(structure.size() + words.size() + 1);
  combined.emplace_back(kStructureSeparator);
  const std::size_t textStart = combined.size();
  for (const WordSpan& word : words)
    combined.push_back(lowered(input.document.substr(word.byteBegin, word.byteEnd - word.byteBegin)));
  GlinerPrompt prompt;
  prompt.ids.reserve(input.document.size() + structure.size());
  for (std::size_t index = 0; index < combined.size(); ++index) {
    const std::size_t position = prompt.ids.size();
    const std::vector<std::int32_t> parts = tokenizePiece(input.tokenizer, combined[index]);
    prompt.ids.insert(prompt.ids.end(), parts.begin(), parts.end());
    if (index >= 4 && index + 3 <= structure.size() && (index - 4) % 2 == 0)
      prompt.queryIndices.push_back(static_cast<std::int64_t>(position));
    if (index >= textStart)
      prompt.wordIndices.push_back(static_cast<std::int64_t>(position));
  }
  prompt.words.reserve(words.size());
  for (const WordSpan& word : words)
    prompt.words.push_back({.begin = word.begin, .end = word.end});
  while (prompt.wordIndices.size() < kWordAxis)
    prompt.wordIndices.push_back(0);
  prompt.wordMask.assign(prompt.wordIndices.size(), 0);
  std::fill_n(prompt.wordMask.begin(), static_cast<std::ptrdiff_t>(words.size()), 1);
  while (prompt.queryIndices.size() < kQueryAxis)
    prompt.queryIndices.push_back(0);
  prompt.queryMask.assign(prompt.queryIndices.size(), 0);
  std::fill_n(prompt.queryMask.begin(), static_cast<std::ptrdiff_t>(input.fields.size()), 1);
  return prompt;
}

std::string_view glinerTypeForTool(std::string_view tool)
{
  for (const ToolType& entry : kToolTypes)
    if (entry.tool == tool)
      return entry.type;
  return {};
}

GlinerExtractor::GlinerExtractor(GlinerExtractorInput input)
    : model_(std::move(input.model)), fallback_(input.fallback), thresholds_(input.thresholds)
{
}

std::optional<std::string> GlinerExtractor::extract(const slots::TextRequest& request) const
{
  const std::string_view type = glinerTypeForTool(request.tool);
  if (type.empty() || model_ == nullptr || model_->status() != EngineStatus::Ready)
    return fallback_ != nullptr ? fallback_->extract(request) : std::nullopt;
  const std::vector<GlinerSpan> spans = model_->spans({.text = request.utterance, .lang = request.lang, .type = type});
  const GlinerSpan* best = nullptr;
  for (const GlinerSpan& span : spans) {
    if (span.field != request.field || span.score < thresholds_.threshold || span.text.empty())
      continue;
    if (best == nullptr || span.score > best->score)
      best = &span;
  }
  if (best == nullptr)
    return fallback_ != nullptr ? fallback_->extract(request) : std::nullopt;
  return best->text;
}

std::unique_ptr<GlinerModel> openGlinerModel(const BundleLoader& bundle, const OnnxOptions& options)
{
  return std::make_unique<GlinerBundleModel>(bundle, options);
}

}
