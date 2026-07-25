#include "unicode-processor.hxx"

#include "onnx-utils.hxx"

#include <algorithm>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Pre-compiled regex patterns (optimization: static const)
// ============================================================================
static const std::regex EMOJI_RE("[\xF0][\x9F][\x80-\xBF][\x80-\xBF]");
static const std::regex SPACE_COMMA_RE(" ,");
static const std::regex SPACE_DOT_RE(" \\.");
static const std::regex SPACE_EXCL_RE(" !");
static const std::regex SPACE_QUESTION_RE(" \\?");
static const std::regex SPACE_SEMICOLON_RE(" ;");
static const std::regex SPACE_COLON_RE(" :");
static const std::regex SPACE_QUOTE_RE(" '");
static const std::regex MULTISPACE_RE(R"(\s+)");

// ============================================================================
// Hangul syllable decomposition constants
// ============================================================================
static const uint32_t HANGUL_SBASE = 0xAC00;
static const uint32_t HANGUL_LBASE = 0x1100;
static const uint32_t HANGUL_VBASE = 0x1161;
static const uint32_t HANGUL_TBASE = 0x11A7;
static const int HANGUL_LCOUNT = 19;
static const int HANGUL_VCOUNT = 21;
static const int HANGUL_TCOUNT = 28;
static const int HANGUL_NCOUNT = HANGUL_VCOUNT * HANGUL_TCOUNT;
static const int HANGUL_SCOUNT = HANGUL_LCOUNT * HANGUL_NCOUNT;

static const std::unordered_map<uint32_t, std::vector<uint16_t>>
    LATIN_DECOMPOSITIONS = {
        {0x00C1, {0x0041, 0x0301}}, {0x00C9, {0x0045, 0x0301}},
        {0x00CD, {0x0049, 0x0301}}, {0x00D3, {0x004F, 0x0301}},
        {0x00DA, {0x0055, 0x0301}}, {0x00E1, {0x0061, 0x0301}},
        {0x00E9, {0x0065, 0x0301}}, {0x00ED, {0x0069, 0x0301}},
        {0x00F3, {0x006F, 0x0301}}, {0x00FA, {0x0075, 0x0301}},
        {0x00C0, {0x0041, 0x0300}}, {0x00C8, {0x0045, 0x0300}},
        {0x00CC, {0x0049, 0x0300}}, {0x00D2, {0x004F, 0x0300}},
        {0x00D9, {0x0055, 0x0300}}, {0x00E0, {0x0061, 0x0300}},
        {0x00E8, {0x0065, 0x0300}}, {0x00EC, {0x0069, 0x0300}},
        {0x00F2, {0x006F, 0x0300}}, {0x00F9, {0x0075, 0x0300}},
        {0x00C2, {0x0041, 0x0302}}, {0x00CA, {0x0045, 0x0302}},
        {0x00CE, {0x0049, 0x0302}}, {0x00D4, {0x004F, 0x0302}},
        {0x00DB, {0x0055, 0x0302}}, {0x00E2, {0x0061, 0x0302}},
        {0x00EA, {0x0065, 0x0302}}, {0x00EE, {0x0069, 0x0302}},
        {0x00F4, {0x006F, 0x0302}}, {0x00FB, {0x0075, 0x0302}},
        {0x00C3, {0x0041, 0x0303}}, {0x00D1, {0x004E, 0x0303}},
        {0x00D5, {0x004F, 0x0303}}, {0x00E3, {0x0061, 0x0303}},
        {0x00F1, {0x006E, 0x0303}}, {0x00F5, {0x006F, 0x0303}},
        {0x00C4, {0x0041, 0x0308}}, {0x00CB, {0x0045, 0x0308}},
        {0x00CF, {0x0049, 0x0308}}, {0x00D6, {0x004F, 0x0308}},
        {0x00DC, {0x0055, 0x0308}}, {0x00E4, {0x0061, 0x0308}},
        {0x00EB, {0x0065, 0x0308}}, {0x00EF, {0x0069, 0x0308}},
        {0x00F6, {0x006F, 0x0308}}, {0x00FC, {0x0075, 0x0308}},
        {0x00C7, {0x0043, 0x0327}}, {0x00E7, {0x0063, 0x0327}},
};

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& str)
{
  size_t start = 0;
  while (start < str.size() &&
         std::isspace(static_cast<unsigned char>(str[start]))) {
    start++;
  }
  size_t end = str.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(str[end - 1]))) {
    end--;
  }
  return str.substr(start, end - start);
}

static void decomposeCharacter(uint32_t codepoint,
                               std::vector<uint16_t>& output)
{
  if (codepoint >= HANGUL_SBASE && codepoint < HANGUL_SBASE + HANGUL_SCOUNT) {
    uint32_t sIndex = codepoint - HANGUL_SBASE;
    uint32_t lIndex = sIndex / HANGUL_NCOUNT;
    uint32_t vIndex = (sIndex % HANGUL_NCOUNT) / HANGUL_TCOUNT;
    uint32_t tIndex = sIndex % HANGUL_TCOUNT;

    output.push_back(static_cast<uint16_t>(HANGUL_LBASE + lIndex));
    output.push_back(static_cast<uint16_t>(HANGUL_VBASE + vIndex));
    if (tIndex > 0) {
      output.push_back(static_cast<uint16_t>(HANGUL_TBASE + tIndex));
    }
    return;
  }

  auto it = LATIN_DECOMPOSITIONS.find(codepoint);
  if (it != LATIN_DECOMPOSITIONS.end()) {
    for (uint16_t cp : it->second) {
      output.push_back(cp);
    }
    return;
  }

  output.push_back(static_cast<uint16_t>(codepoint & 0xFFFF));
}

// ============================================================================
// UnicodeProcessor
// ============================================================================

UnicodeProcessor::UnicodeProcessor(const std::string& unicodeIndexerJsonPath)
{
  indexer_ = loadJsonInt64(unicodeIndexerJsonPath);
}

std::string UnicodeProcessor::preprocessText(const std::string& text,
                                             const std::string& lang) const
{
  std::string result;
  result.reserve(text.size() * 12 / 10); // 20% extra for tag wrapping

  result = text;

  // --- Single-pass character replacements (optimized: table-driven) ---
  struct Replacement
  {
    const char* from;
    const char* to;
  };

  static const Replacement replacements[] = {
      {"\u2013", "-"}, // en dash
      {"\u2011", "-"}, // non-breaking hyphen
      {"\u2014", "-"}, // em dash
      {"_", " "},      {"\u201C", "\""}, {"\u201D", "\""}, {"\u2018", "'"},
      {"\u2019", "'"}, {"\u00B4", "'"}, // acute accent
      {"`", "'"},      {"[", " "},       {"]", " "},       {"|", " "},
      {"/", " "},      {"#", " "},       {"\u2192", " "}, // right arrow
      {"\u2190", " "},                                    // left arrow
  };

  for (const auto& repl : replacements) {
    size_t pos = 0;
    while ((pos = result.find(repl.from, pos)) != std::string::npos) {
      result.replace(pos, std::strlen(repl.from), repl.to);
      pos += std::strlen(repl.to);
    }
  }

  // Remove emojis
  result = std::regex_replace(result, EMOJI_RE, "");

  // Remove special symbols
  static const char* specialSymbols[] = {"\u2665", "\u2606", "\u2661", "\u00A9",
                                         "\\"};
  for (const char* symbol : specialSymbols) {
    size_t pos = 0;
    while ((pos = result.find(symbol, pos)) != std::string::npos) {
      result.erase(pos, std::strlen(symbol));
    }
  }

  // Replace known expressions
  static const Replacement exprReplacements[] = {
      {"@", " at "},
      {"e.g.,", "for example, "},
      {"i.e.,", "that is, "},
  };

  for (const auto& repl : exprReplacements) {
    size_t pos = 0;
    while ((pos = result.find(repl.from, pos)) != std::string::npos) {
      result.replace(pos, std::strlen(repl.from), repl.to);
      pos += std::strlen(repl.to);
    }
  }

  // Fix spacing around punctuation
  result = std::regex_replace(result, SPACE_COMMA_RE, ",");
  result = std::regex_replace(result, SPACE_DOT_RE, ".");
  result = std::regex_replace(result, SPACE_EXCL_RE, "!");
  result = std::regex_replace(result, SPACE_QUESTION_RE, "?");
  result = std::regex_replace(result, SPACE_SEMICOLON_RE, ";");
  result = std::regex_replace(result, SPACE_COLON_RE, ":");
  result = std::regex_replace(result, SPACE_QUOTE_RE, "'");

  // Remove duplicate quotes
  while (result.find("\"\"") != std::string::npos) {
    result.replace(result.find("\"\""), 2, "\"");
  }
  while (result.find("''") != std::string::npos) {
    result.replace(result.find("''"), 2, "'");
  }
  while (result.find("``") != std::string::npos) {
    result.replace(result.find("``"), 2, "`");
  }

  // Remove extra spaces
  result = std::regex_replace(result, MULTISPACE_RE, " ");
  result = trim(result);

  // Add period if text doesn't end with punctuation
  if (!result.empty()) {
    char lastChar = result.back();
    bool endsWithPunct =
        (lastChar == '.' || lastChar == '!' || lastChar == '?' ||
         lastChar == ';' || lastChar == ':' || lastChar == ',' ||
         lastChar == '\'' || lastChar == '"' || lastChar == ')' ||
         lastChar == ']' || lastChar == '}' || lastChar == '>');

    if (!endsWithPunct && result.size() >= 3) {
      std::string lastThree = result.substr(result.size() - 3);
      if (lastThree == "\u2026" || lastThree == "\u3002" ||
          lastThree == "\u300D" || lastThree == "\u300F" ||
          lastThree == "\u3011" || lastThree == "\u3009" ||
          lastThree == "\u300B" || lastThree == "\u203A" ||
          lastThree == "\u00BB" || lastThree == "\u201C" ||
          lastThree == "\u201D" || lastThree == "\u2018" ||
          lastThree == "\u2019") {
        endsWithPunct = true;
      }
    }

    if (!endsWithPunct) {
      result += ".";
    }
  }

  // Validate language
  bool validLang = false;
  for (const auto& available : supportedLangCodes()) {
    if (lang == available) {
      validLang = true;
      break;
    }
  }
  if (!validLang) {
    throw std::runtime_error("Invalid language: " + lang);
  }

  // Wrap with language tags
  result = "<" + lang + ">" + result + "</" + lang + ">";

  return result;
}

std::vector<uint16_t>
UnicodeProcessor::textToUnicodeValues(const std::string& text)
{
  std::vector<uint16_t> unicodeValues;
  unicodeValues.reserve(text.size());
  size_t i = 0;

  while (i < text.size()) {
    uint32_t codepoint = 0;
    unsigned char c = static_cast<unsigned char>(text[i]);

    if ((c & 0x80) == 0) {
      codepoint = c;
      i += 1;
    }
    else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
      codepoint = (c & 0x1F) << 6;
      codepoint |= (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      i += 2;
    }
    else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
      codepoint = (c & 0x0F) << 12;
      codepoint |= (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6;
      codepoint |= (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      i += 3;
    }
    else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
      codepoint = (c & 0x07) << 18;
      codepoint |= (static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12;
      codepoint |= (static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6;
      codepoint |= (static_cast<unsigned char>(text[i + 3]) & 0x3F);
      i += 4;
    }
    else {
      i += 1;
      continue;
    }

    decomposeCharacter(codepoint, unicodeValues);
  }

  return unicodeValues;
}

std::vector<std::vector<std::vector<float>>>
UnicodeProcessor::textMask(const std::vector<int64_t>& textIdsLengths) const
{
  return lengthToMask(textIdsLengths);
}

void UnicodeProcessor::process(
    const std::vector<std::string>& textList,
    const std::vector<std::string>& langList,
    std::vector<std::vector<int64_t>>& textIds,
    std::vector<std::vector<std::vector<float>>>& textMask) const
{
  std::vector<std::string> processedTexts;
  processedTexts.reserve(textList.size());
  for (size_t i = 0; i < textList.size(); i++) {
    processedTexts.push_back(preprocessText(textList[i], langList[i]));
  }

  std::vector<std::vector<uint16_t>> allUnicodeVals;
  allUnicodeVals.reserve(processedTexts.size());
  std::vector<int64_t> textIdsLengths;
  textIdsLengths.reserve(processedTexts.size());
  for (const auto& text : processedTexts) {
    auto unicodeVals = textToUnicodeValues(text);
    textIdsLengths.push_back(static_cast<int64_t>(unicodeVals.size()));
    allUnicodeVals.push_back(std::move(unicodeVals));
  }

  int64_t maxLen =
      *std::max_element(textIdsLengths.begin(), textIdsLengths.end());

  textIds.resize(textList.size());
  for (size_t i = 0; i < allUnicodeVals.size(); i++) {
    textIds[i].resize(maxLen, 0);
    const auto& unicodeVals = allUnicodeVals[i];
    for (size_t j = 0; j < unicodeVals.size(); j++) {
      if (unicodeVals[j] < indexer_.size()) {
        textIds[i][j] = indexer_[unicodeVals[j]];
      }
    }
  }

  textMask = this->textMask(textIdsLengths);
}
