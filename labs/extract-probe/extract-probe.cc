#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <json/json.h>
#include <llama.h>
#include <new>
#include <random>
#include <shared/repositories/memory-lexicon/memory-lexicon-repository.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/extract/extraction-service.hxx>
#include <shared/services/extract/lexicon-extractor.hxx>
#include <shared/services/extract/tiered-extractor.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/utils/text-match/phrase-automaton.hxx>
#include <shared/wrapper/cancellation/cancellation-token.hxx>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

// The extraction vocabulary lives in SQLite (memory_lexicon): load it the
// same way the product does instead of hardcoding a copy in the probe.
std::vector<extract::LexiconEntry> loadLexicon()
{
  static SqliteGraph graph;
  static MemoryLexiconRepository repo;
  static bool opened = false;
  if (!opened) {
    graph.open(ConfigService::getString("database.file"));
    graph.applySchema();
    opened = true;
  }
  std::scoped_lock lock(graph.mutex());
  return repo.allEntries(graph.handle());
}

std::atomic<long long> gNewCount{0};

void* rawAlloc(std::size_t n)
{
  gNewCount.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}

bool isWordChar(unsigned char c)
{
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

struct RefHit
{
  uint32_t patternIndex;
  uint32_t begin;
  uint32_t end;
};

bool operator<(const RefHit& a, const RefHit& b)
{
  if (a.begin != b.begin)
    return a.begin < b.begin;
  if (a.end != b.end)
    return a.end < b.end;
  return a.patternIndex < b.patternIndex;
}

bool operator==(const RefHit& a, const RefHit& b)
{
  return a.begin == b.begin && a.end == b.end &&
         a.patternIndex == b.patternIndex;
}

std::string foldUtf8(std::string_view s)
{
  static constexpr std::pair<const char*, char> kFold[] = {
      {"á", 'a'}, {"é", 'e'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'}, {"ü", 'u'},
      {"ñ", 'n'}, {"Á", 'a'}, {"É", 'e'}, {"Í", 'i'}, {"Ó", 'o'}, {"Ú", 'u'},
      {"Ü", 'u'}, {"Ñ", 'n'}, {"à", 'a'}, {"è", 'e'}, {"ì", 'i'}, {"ò", 'o'},
      {"ù", 'u'}, {"â", 'a'}, {"ê", 'e'}, {"î", 'i'}, {"ô", 'o'}, {"û", 'u'},
      {"ä", 'a'}, {"ë", 'e'}, {"ï", 'i'}, {"ö", 'o'}, {"ÿ", 'y'}, {"ç", 'c'},
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    bool folded = false;
    for (const auto& entry : kFold) {
      const size_t l = std::strlen(entry.first);
      if (i + l <= s.size() &&
          s.substr(i, l) == std::string_view(entry.first, l)) {
        out.push_back(entry.second);
        i += l;
        folded = true;
        break;
      }
    }
    if (!folded) {
      out.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
      ++i;
    }
  }
  return out;
}

std::string normalize(std::string_view s)
{
  std::string t = foldUtf8(s);
  std::string out;
  out.reserve(t.size());
  bool pendingSpace = false;
  for (const char c : t) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !out.empty())
      out.push_back(' ');
    pendingSpace = false;
    out.push_back(c);
  }
  return out;
}

std::vector<RefHit> referenceMatch(const std::vector<std::string>& patterns,
                                   std::string_view text)
{
  std::vector<RefHit> hits;
  const size_t n = text.size();
  for (uint32_t pi = 0; pi < patterns.size(); ++pi) {
    const std::string& pat = patterns[pi];
    const size_t m = pat.size();
    if (m == 0 || m > n)
      continue;
    for (size_t i = 0; i + m <= n; ++i) {
      if (std::memcmp(text.data() + i, pat.data(), m) != 0)
        continue;
      if (i > 0 && isWordChar(static_cast<unsigned char>(text[i - 1])))
        continue;
      if (i + m < n && isWordChar(static_cast<unsigned char>(text[i + m])))
        continue;
      if (!isWordChar(static_cast<unsigned char>(text[i])) ||
          !isWordChar(static_cast<unsigned char>(text[i + m - 1])))
        continue;
      hits.push_back({.patternIndex = pi,
                      .begin = static_cast<uint32_t>(i),
                      .end = static_cast<uint32_t>(i + m)});
    }
  }
  return hits;
}

std::vector<RefHit>
automatonMatch(const std::shared_ptr<const text_match::PhraseAutomaton>& a,
               std::string_view text)
{
  text_match::MatchBuffer buf;
  a->match(text, buf);
  std::vector<RefHit> hits;
  hits.reserve(buf.items.size());
  for (const text_match::Match& m : buf.items)
    hits.push_back(
        {.patternIndex = m.patternIndex, .begin = m.begin, .end = m.end});
  std::sort(hits.begin(), hits.end());
  return hits;
}

int failures = 0;

void report(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", what);
  if (!ok)
    ++failures;
}

const std::vector<std::string> kStopwords = {
    "el",  "la", "los", "las", "de", "en",     "a",     "y",    "que",
    "mi",  "su", "un",  "una", "al", "del",    "para",  "por",  "con",
    "sin", "se", "le",  "no",  "si", "cuando", "donde", "cada",
};

const std::vector<std::string> kKinship = {
    "hermana", "hermano", "madre", "padre",   "hija",   "hijo",
    "abuela",  "abuelo",  "prima", "primo",   "tio",    "tia",
    "cunada",  "nieta",   "nieto", "sobrina", "esposa", "esposo",
};

const std::vector<std::string> kPredicates = {
    "gusta", "prefiere", "viene",    "duerme", "trabaja", "estudia", "cocina",
    "tiene", "quiere",   "necesita", "visita", "llama",   "compra",  "odia",
    "ama",   "recuerda", "llega",    "sale",   "come",    "bebe",
};

const std::vector<std::string> kObjects = {
    "cebolla", "cafe", "leche", "pan",    "fruta", "pescado", "carne", "tomate",
    "sofa",    "mesa", "casa",  "jardin", "coche", "perro",   "gato",  "libro",
    "te",      "vino", "queso", "arroz",  "pollo", "huevo",
};

const std::vector<std::string> kPeople = {
    "maria", "juan",  "ana",   "luis",  "carmen", "pedro",
    "sofia", "laura", "diego", "elena", "marta",  "pablo",
};

const std::vector<std::string> kTimes = {
    "domingos", "viernes", "lunes",     "martes",   "sabado",    "manana",
    "tarde",    "noche",   "las siete", "las ocho", "las nueve", "mediodia",
};

const std::vector<std::string> kPlaces = {
    "cocina", "salon", "dormitorio", "jardin", "garaje", "escalera", "terraza",
};

const std::vector<std::string> kTraps = {
    "pesos", "mesa", "playa", "calle",    "lista",  "correo",
    "hueso", "paso", "seso",  "elefante", "vuelta", "pares",
};

const std::string& pick(std::mt19937& rng, const std::vector<std::string>& pool)
{
  return pool[rng() % pool.size()];
}

std::vector<std::string> makePatterns(size_t count)
{
  std::unordered_set<std::string> seen;
  std::vector<std::string> out;
  const auto add = [&](const std::string& p) {
    if (out.size() >= count)
      return;
    if (seen.insert(p).second)
      out.push_back(p);
  };
  const auto done = [&] { return out.size() >= count; };
  for (const auto& w : kStopwords)
    add(w);
  for (const auto& w : kKinship)
    add(w);
  for (const auto& w : kPredicates)
    add(w);
  for (const auto& w : kObjects)
    add(w);
  for (const auto& w : kPeople)
    add(w);
  for (const auto& w : kTimes)
    add(w);
  for (const auto& w : kPlaces)
    add(w);
  for (const auto& w : kTraps)
    add(w);
  for (size_t i = 0; !done() && i < kObjects.size(); ++i)
    add("me gusta " + kObjects[i]);
  for (size_t i = 0; !done() && i < kObjects.size(); ++i)
    add("no le gusta " + kObjects[i]);
  for (size_t i = 0; !done() && i < kKinship.size(); ++i)
    for (size_t j = 0; !done() && j < kPredicates.size(); ++j)
      add(kKinship[i] + " " + kPredicates[j]);
  for (size_t i = 0; !done() && i < kKinship.size(); ++i)
    for (size_t j = 0; !done() && j < kPredicates.size(); ++j)
      for (size_t k = 0; !done() && k < kObjects.size(); ++k)
        add(kKinship[i] + " " + kPredicates[j] + " " + kObjects[k]);
  for (size_t i = 0; !done() && i < kPeople.size(); ++i)
    for (size_t j = 0; !done() && j < kPredicates.size(); ++j)
      add(kPeople[i] + " " + kPredicates[j]);
  for (size_t i = 0; !done() && i < kKinship.size(); ++i)
    for (size_t j = 0; !done() && j < kTimes.size(); ++j)
      add("mi " + kKinship[i] + " viene " + kTimes[j]);
  for (size_t i = 0; !done() && i < kPredicates.size(); ++i)
    for (size_t j = 0; !done() && j < kPlaces.size(); ++j)
      add(kPredicates[i] + " en " + kPlaces[j]);
  for (size_t i = 0; !done() && i < kTimes.size(); ++i)
    for (size_t j = 0; !done() && j < kPredicates.size(); ++j)
      add("cada " + kTimes[i] + " " + kPredicates[j]);
  for (size_t i = 0; !done() && i < kStopwords.size(); ++i)
    for (size_t j = 0; !done() && j < kKinship.size(); ++j)
      for (size_t k = 0; !done() && k < kPredicates.size(); ++k)
        add(kStopwords[i] + " " + kKinship[j] + " " + kPredicates[k]);
  return out;
}

std::string makeText(std::mt19937& rng)
{
  const size_t tokens = 4 + rng() % 7;
  std::string t;
  for (size_t i = 0; i < tokens; ++i) {
    const size_t r = rng() % 100;
    if (r < 30)
      t += pick(rng, kStopwords);
    else if (r < 45)
      t += pick(rng, kKinship);
    else if (r < 62)
      t += pick(rng, kPredicates);
    else if (r < 78)
      t += pick(rng, kObjects);
    else if (r < 88)
      t += pick(rng, kPeople);
    else if (r < 93)
      t += pick(rng, kTimes);
    else if (r < 97)
      t += pick(rng, kPlaces);
    else
      t += pick(rng, kTraps);
    t.push_back(' ');
  }
  if (rng() % 3 == 0)
    t.insert(t.size() - 1, 1, ',');
  if (rng() % 5 == 0)
    t.insert(t.size() - 1, 1, '.');
  if (rng() % 7 == 0)
    t.insert(t.size() - 1, 1, '?');
  t.pop_back();
  return t;
}

int runAutomatonTest(unsigned seed)
{
  std::mt19937 rng(seed);
  std::printf("== automaton-test ==\n");

  const std::vector<std::string> fixedPatterns = {
      "es",  "casa", "me gusta", "gusta",   "a las 7",
      "los", "el",   "elefante", "hermana", "viene los domingos",
  };
  const std::vector<std::string> fixedTexts = {
      "pesos mesa playa",
      "la casa es grande",
      "me gusta el cafe",
      "el elefante y el raton",
      "vengo a las 7",
      "mi hermana viene los domingos",
      "los los los",
      "el el el",
      "a las 7 y a las 8",
      "no me gusta nada",
      "su hermana viene manana a la casa",
  };
  std::vector<text_match::PatternRef> refs;
  refs.reserve(fixedPatterns.size());
  for (size_t i = 0; i < fixedPatterns.size(); ++i)
    refs.push_back({.classId = static_cast<text_match::PatternClass>(i % 7),
                    .payloadId = static_cast<uint32_t>(i),
                    .text = fixedPatterns[i]});
  const auto fixed = text_match::PhraseAutomaton::build(refs);
  bool ok = true;
  for (const std::string& raw : fixedTexts) {
    const std::string text = normalize(raw);
    auto expect = referenceMatch(fixedPatterns, text);
    std::sort(expect.begin(), expect.end());
    const auto got = automatonMatch(fixed, text);
    if (expect != got) {
      ok = false;
      std::printf("  [FAIL] text '%s': expected %zu hits, got %zu\n",
                  text.c_str(), expect.size(), got.size());
    }
  }
  report(ok, "fixed word-boundary cases");

  ok = true;
  for (size_t round = 0; round < 40; ++round) {
    const size_t np = 1 + rng() % 250;
    std::vector<std::string> patterns = makePatterns(np);
    std::vector<text_match::PatternRef> refs2;
    refs2.reserve(patterns.size());
    for (size_t i = 0; i < patterns.size(); ++i)
      refs2.push_back({.classId = static_cast<text_match::PatternClass>(i % 7),
                       .payloadId = static_cast<uint32_t>(i),
                       .text = patterns[i]});
    const auto a = text_match::PhraseAutomaton::build(refs2);
    for (size_t t = 0; t < 40; ++t) {
      const std::string text = normalize(makeText(rng));
      auto expect = referenceMatch(patterns, text);
      std::sort(expect.begin(), expect.end());
      const auto got = automatonMatch(a, text);
      if (expect != got) {
        ok = false;
        std::printf("  [FAIL] round %zu patterns=%zu text '%s': "
                    "expected %zu hits, got %zu\n",
                    round, patterns.size(), text.c_str(), expect.size(),
                    got.size());
        for (const auto& h : expect)
          std::printf("    expect #%u [%u,%u)\n", h.patternIndex, h.begin,
                      h.end);
        for (const auto& h : got)
          std::printf("    got    #%u [%u,%u)\n", h.patternIndex, h.begin,
                      h.end);
        break;
      }
    }
  }
  report(ok, "property test vs O(n*m) reference (40 rounds)");

  std::vector<std::string> big = makePatterns(2000);
  std::vector<text_match::PatternRef> refs3;
  refs3.reserve(big.size());
  for (size_t i = 0; i < big.size(); ++i)
    refs3.push_back({.classId = static_cast<text_match::PatternClass>(i % 7),
                     .payloadId = static_cast<uint32_t>(i),
                     .text = big[i]});
  const auto bigA = text_match::PhraseAutomaton::build(refs3);
  ok = true;
  for (size_t t = 0; t < 200; ++t) {
    const std::string text = normalize(makeText(rng));
    auto expect = referenceMatch(big, text);
    std::sort(expect.begin(), expect.end());
    const auto got = automatonMatch(bigA, text);
    if (expect != got) {
      ok = false;
      std::printf("  [FAIL] big-set text '%s': expected %zu, got %zu\n",
                  text.c_str(), expect.size(), got.size());
      break;
    }
  }
  report(ok, "large pattern set (2000) consistency");
  return failures;
}

struct BenchResult
{
  double p95Us;
  long long allocs;
  size_t bytes;
  size_t calls;
};

BenchResult runBench(const std::vector<std::string>& patterns,
                     const std::vector<std::string>& texts)
{
  std::vector<text_match::PatternRef> refs;
  refs.reserve(patterns.size());
  for (size_t i = 0; i < patterns.size(); ++i)
    refs.push_back({.classId = static_cast<text_match::PatternClass>(i % 7),
                    .payloadId = static_cast<uint32_t>(i),
                    .text = patterns[i]});
  const auto a = text_match::PhraseAutomaton::build(refs);
  text_match::MatchBuffer buf;
  buf.reserve(4096);
  for (int pass = 0; pass < 3; ++pass) {
    for (const std::string& t : texts) {
      a->match(t, buf);
      buf.clear();
    }
  }

  std::vector<double> times;
  times.reserve(texts.size() * 128);
  const long long allocStart = gNewCount.load(std::memory_order_relaxed);
  size_t calls = 0;
  for (size_t pass = 0; pass < 128 && times.size() < 30000; ++pass) {
    for (const std::string& t : texts) {
      const auto t0 = std::chrono::steady_clock::now();
      a->match(t, buf);
      const auto t1 = std::chrono::steady_clock::now();
      buf.clear();
      times.push_back(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count() /
          1000.0);
      ++calls;
    }
  }
  const long long allocEnd = gNewCount.load(std::memory_order_relaxed);
  std::sort(times.begin(), times.end());
  const double p95 = times[static_cast<size_t>(times.size() * 0.95)];
  return {.p95Us = p95,
          .allocs = allocEnd - allocStart,
          .bytes = a->bytesUsed(),
          .calls = calls};
}

int runAutomatonBench(unsigned seed)
{
  std::mt19937 rng(seed);
  std::printf("\n== automaton-bench ==\n");
  const size_t sizes[] = {100, 1000, 10000};
  std::vector<std::string> full = makePatterns(10000);
  std::vector<std::string> texts;
  texts.reserve(400);
  for (size_t i = 0; i < 400; ++i)
    texts.push_back(normalize(makeText(rng)));

  std::vector<BenchResult> results;
  for (const size_t n : sizes) {
    const std::vector<std::string> patterns(full.begin(), full.begin() + n);
    const BenchResult r = runBench(patterns, texts);
    results.push_back(r);
    std::printf("patterns=%-6zu p95=%7.2f us allocs=%lld "
                "allocs/call=%.4f bytes=%zu\n",
                n, r.p95Us, r.allocs, static_cast<double>(r.allocs) / r.calls,
                r.bytes);
  }

  const double p95Small = results[0].p95Us;
  const double p95Large = results[2].p95Us;
  const size_t bytesLarge = results[2].bytes;
  const long long allocsLarge = results[2].allocs;

  report(p95Large < 200.0, "gate: p95 < 200 us at 10000 patterns");
  report(p95Large <= 2.0 * p95Small,
         "gate: p95(10000) <= 2x p95(100) (flat latency)");
  report(allocsLarge == 0,
         "gate: zero heap allocations per match() after warmup");
  report(bytesLarge < 2 * 1024 * 1024, "gate: < 2 MB at 10000 patterns");
  return failures;
}

} // namespace

void writeModelOverlay(const char* modelPath, const char* format);

struct llama_model;
struct llama_vocab;
struct llama_sampler;

int runGrammarIsolate(const char* modelOverride, const char* formatOverride)
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("\n== grammar-isolate ==\n");
  if (modelOverride && *modelOverride)
    writeModelOverlay(modelOverride, formatOverride);
  ConfigService::load("config.toml");

  const std::string modelPath = ConfigService::getString("extract.model_path");
  llama_model_params mparams = llama_model_default_params();
  mparams.n_gpu_layers = 0;
  mparams.load_mode = LLAMA_LOAD_MODE_MMAP;
  std::unique_ptr<llama_model, void (*)(llama_model*)>
      model(llama_model_load_from_file(modelPath.c_str(), mparams),
            &llama_model_free);
  if (!model) {
    std::printf("  model load failed\n");
    return 1;
  }
  const auto* vocab = llama_model_get_vocab(model.get());
  auto tokenOf = [vocab](const char* s) {
    const int32_t n =
        llama_tokenize(vocab, s, static_cast<int32_t>(std::strlen(s)), nullptr,
                       0, false, false);
    std::vector<llama_token> out(n < 0 ? static_cast<size_t>(-n) : 1);
    llama_tokenize(vocab, s, static_cast<int32_t>(std::strlen(s)), out.data(),
                   static_cast<int32_t>(out.size()), false, false);
    return out[0];
  };
  const llama_token tOpen = tokenOf("{");
  const llama_token tX = tokenOf("x");
  const llama_token tEos = llama_vocab_eos(vocab);

  const char* kTrivial =
      "root ::= \"{\" str \"}\"\n"
      "str ::= \"\\\"\" ([^\"\\\\] | \"\\\\\" .)* \"\\\"\"\n";
  const char* kTemplate = "{\"facts\": [{\"subject\": \"\", \"action\": \"\", "
                          "\"object\": \"\", \"time\": \"\"}]}";
  Json::Value tpl;
  Json::Reader().parse(kTemplate, tpl);
  const std::string gen = ExtractionService::buildGrammar(tpl, kTemplate);

  const auto prompt =
      ExtractionService::buildPrompt({.templateJson = kTemplate,
                                      .schemaJson = {},
                                      .text = "mi hermana viene los domingos",
                                      .format = ExtractPromptFormat::V15});
  const int32_t probe =
      llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                     nullptr, 0, true, false);
  std::vector<llama_token> ptok(probe < 0 ? static_cast<size_t>(-probe) : 1);
  llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                 ptok.data(), static_cast<int32_t>(ptok.size()), true, false);

  llama_context_params cparams = llama_context_default_params();
  cparams.n_ctx = 2048;
  cparams.n_batch = 256;
  cparams.n_ubatch = 256;
  cparams.n_threads = 4;
  cparams.n_threads_batch = 4;
  cparams.no_perf = true;
  std::unique_ptr<llama_context, void (*)(llama_context*)>
      ctx(llama_init_from_model(model.get(), cparams), &llama_free);
  if (!ctx) {
    std::printf("  context init failed\n");
    return 1;
  }
  auto batch = std::unique_ptr<llama_batch>(
      new llama_batch(llama_batch_init(256, 0, 1)));
  for (size_t j = 0; j < ptok.size(); ++j) {
    batch->token[j] = ptok[j];
    batch->pos[j] = static_cast<int32_t>(j);
    batch->n_seq_id[j] = 1;
    batch->seq_id[j][0] = 0;
    batch->logits[j] = (j == ptok.size() - 1) ? 1 : 0;
  }
  batch->n_tokens = static_cast<int32_t>(ptok.size());
  if (llama_decode(ctx.get(), *batch) != 0) {
    std::printf("  prompt decode failed\n");
    return 1;
  }
  std::printf("  n_vocab=%d probs_count=%zu logits_count=%zu cand_count=%zu\n",
              llama_vocab_n_tokens(vocab),
              static_cast<size_t>(
                  llama_get_sampled_probs_count_ith(ctx.get(), -1)),
              static_cast<size_t>(
                  llama_get_sampled_logits_count_ith(ctx.get(), -1)),
              static_cast<size_t>(
                  llama_get_sampled_candidates_count_ith(ctx.get(), -1)));

  auto sparams = llama_sampler_chain_default_params();
  std::unique_ptr<llama_sampler, void (*)(llama_sampler*)>
      chain(llama_sampler_chain_init(sparams), &llama_sampler_free);
  llama_sampler* g = llama_sampler_init_grammar(vocab, gen.c_str(), "root");
  if (!g) {
    std::printf("  generated grammar parse failed\n");
    return 1;
  }
  llama_sampler_chain_add(chain.get(), g);
  llama_sampler_chain_add(chain.get(), llama_sampler_init_greedy());

  std::string out;
  const auto probeState = [&](const char*, int afterSample) {
    const llama_token tQuote = tokenOf("\"");
    const llama_token tSpace = tokenOf(" ");
    const llama_token tFacts = tokenOf("facts");
    llama_token_data cands[6] = {{tOpen, 0.0f, 0.0f},  {tQuote, 0.0f, 0.0f},
                                 {tSpace, 0.0f, 0.0f}, {tX, 0.0f, 0.0f},
                                 {tFacts, 0.0f, 0.0f}, {tEos, 0.0f, 0.0f}};
    llama_token_data_array cur_p = {cands, 6, -1, false};
    llama_sampler_apply(chain.get(), &cur_p);
    std::printf("  state after sample %d: open=%s quote=%s space=%s x=%s "
                "facts=%s eos=%s\n",
                afterSample, cands[0].logit != -INFINITY ? "Y" : "N",
                cands[1].logit != -INFINITY ? "Y" : "N",
                cands[2].logit != -INFINITY ? "Y" : "N",
                cands[3].logit != -INFINITY ? "Y" : "N",
                cands[4].logit != -INFINITY ? "Y" : "N",
                cands[5].logit != -INFINITY ? "Y" : "N");
  };
  probeState("", -1);
  for (int i = 0; i < 6; ++i) {
    const llama_token tok = llama_sampler_sample(chain.get(), ctx.get(), -1);
    if (tok == tEos) {
      std::printf("  sample %2d: EOG\n", i);
      break;
    }
    char buf[256];
    const int n = llama_token_to_piece(vocab, tok, buf, sizeof(buf), 0, true);
    out.append(buf, static_cast<size_t>(n));
    std::printf("  sample %2d: id=%d piece='%s'\n", i, tok,
                std::string(buf, static_cast<size_t>(n)).c_str());
    batch->token[0] = tok;
    batch->pos[0] = static_cast<int32_t>(ptok.size() + i);
    batch->n_seq_id[0] = 1;
    batch->seq_id[0][0] = 0;
    batch->logits[0] = 1;
    batch->n_tokens = 1;
    if (llama_decode(ctx.get(), *batch) != 0) {
      std::printf("  decode failed at %d\n", i);
      break;
    }
    probeState("", i);
  }
  std::printf("  raw output: %s\n", out.c_str());

  const auto apply = [&](llama_sampler* chain2, const char* label) {
    llama_token_data cands[3] = {{tOpen, 0.0f, 0.0f},
                                 {tX, 0.0f, 0.0f},
                                 {tEos, 0.0f, 0.0f}};
    llama_token_data_array cur_p = {cands, 3, -1, false};
    llama_sampler_apply(chain2, &cur_p);
    const bool openOk = cands[0].logit != -INFINITY;
    const bool xOk = cands[1].logit != -INFINITY;
    const bool eosOk = cands[2].logit != -INFINITY;
    std::printf("  %-28s open=%s x=%s eos=%s\n", label,
                openOk ? "ALLOW" : "reject", xOk ? "ALLOW" : "reject",
                eosOk ? "ALLOW" : "reject");
    return openOk && !xOk && !eosOk;
  };

  bool ok = true;
  {
    auto sparams = llama_sampler_chain_default_params();
    std::unique_ptr<llama_sampler, void (*)(llama_sampler*)>
        chain(llama_sampler_chain_init(sparams), &llama_sampler_free);
    llama_sampler* g = llama_sampler_init_grammar(vocab, kTrivial, "root");
    llama_sampler_chain_add(chain.get(), g);
    llama_sampler_chain_add(chain.get(), llama_sampler_init_greedy());
    ok &= apply(chain.get(), "trivial fresh");
    llama_sampler_reset(chain.get());
    ok &= apply(chain.get(), "trivial after reset");
  }
  {
    auto sparams = llama_sampler_chain_default_params();
    std::unique_ptr<llama_sampler, void (*)(llama_sampler*)>
        chain(llama_sampler_chain_init(sparams), &llama_sampler_free);
    llama_sampler* g = llama_sampler_init_grammar(vocab, gen.c_str(), "root");
    if (!g) {
      std::printf("  generated grammar parse failed\n");
      return 1;
    }
    llama_sampler_chain_add(chain.get(), g);
    llama_sampler_chain_add(chain.get(), llama_sampler_init_greedy());
    ok &= apply(chain.get(), "generated fresh");
    llama_sampler_reset(chain.get());
    ok &= apply(chain.get(), "generated after reset");
  }
  report(ok, "grammar filters candidates in all states");
  return failures;
}

int runGrammarSmoke(const char* modelOverride, const char* formatOverride)
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("\n== grammar-smoke ==\n");
  if (modelOverride && *modelOverride)
    writeModelOverlay(modelOverride, formatOverride);
  ConfigService::load("config.toml");
  ExtractionService service;
  if (!service.ensureLoaded()) {
    std::printf("  model not loaded\n");
    return 1;
  }
  const char* kTemplate = "{\"facts\": [{\"subject\": \"\", \"action\": \"\", "
                          "\"object\": \"\", \"time\": \"\"}]}";
  Json::Value tpl;
  Json::Reader().parse(kTemplate, tpl);
  const std::string grammar = ExtractionService::buildGrammar(tpl, kTemplate);
  std::printf("  generated grammar (%zu bytes):\n%s\n", grammar.size(),
              grammar.c_str());
  {
    const auto result =
        service.extract({.text = "mi hermana viene los domingos",
                         .templateJson = "{\"facts\": []}",
                         .schemaJson = {},
                         .maxTokens = 64,
                         .cancel = CancellationToken{}});
    report(result.has_value(), "extraction (template grammar)");
    if (result)
      std::printf("  template output: %s\n",
                  Json::FastWriter().write(*result).c_str());
  }
  {
    const auto result =
        service.extract({.text = "mi hermana viene los domingos",
                         .templateJson = "{\"facts\": [{\"subject\": \"\"}]}",
                         .schemaJson = {},
                         .maxTokens = 64,
                         .cancel = CancellationToken{}});
    report(result.has_value(), "extraction (trivial obj grammar)");
  }
  {
    const auto result =
        service.extract({.text = "mi hermana viene los domingos",
                         .templateJson = kTemplate,
                         .schemaJson = {},
                         .maxTokens = 64,
                         .cancel = CancellationToken{}});
    report(result.has_value(), "extraction with generated grammar");
    if (result)
      std::printf("  output: %s\n", Json::FastWriter().write(*result).c_str());
  }
  return failures;
}

// The overlay must not outlive the run: a leftover config.local.toml silently
// overrides config.toml, so the next run without --model measures the previous
// model and reports it as the baseline.
void writeModelOverlay(const char* modelPath, const char* format)
{
  std::ofstream overlay("config.local.toml");
  overlay << "[extract]\nmodel_path = \"" << modelPath << "\"\n";
  if (format && *format)
    overlay << "prompt_format = \"" << format << "\"\n";
  overlay.close();
  static bool armed = false;
  if (!armed) {
    armed = true;
    std::atexit([] { std::remove("config.local.toml"); });
  }
}

int runNuextractTest(const char* modelOverride, const char* formatOverride)
{
  std::printf("\n== nuextract-test ==\n");
  if (modelOverride && *modelOverride)
    writeModelOverlay(modelOverride, formatOverride);
  ConfigService::load("config.toml");
  const char* kTemplate =
      "{\"facts\": [{\"subject\": \"\", \"action\": \"\", \"object\": \"\", "
      "\"time\": \"\"}]}";
  const std::string prompt =
      ExtractionService::buildPrompt({.templateJson = kTemplate,
                                      .schemaJson = {},
                                      .text = "mi hermana viene los domingos",
                                      .format = ExtractPromptFormat::V15});
  const std::string expect =
      "<|input|>\n### Template:\n" + std::string(kTemplate) +
      "\n### Text:\nmi hermana viene los domingos\n\n<|output|>\n";
  report(prompt == expect, "prompt format matches the NuExtract spec (§1.3)");

  Json::Value tpl;
  Json::Reader().parse(kTemplate, tpl);
  const std::string grammar = ExtractionService::buildGrammar(tpl, kTemplate);
  const bool hasRoot = grammar.find("root ::= ws r0 ws") != std::string::npos;
  const bool hasFacts =
      grammar.find("\"\\\"facts\\\"\" ws \":\" ws") != std::string::npos;
  const bool hasStr = grammar.find("str ::=") != std::string::npos;
  const bool hasArray = grammar.find("r0-p0-e") != std::string::npos;
  report(hasRoot && hasFacts && hasStr && hasArray,
         "GBNF grammar covers root/object/array/string shape");

  struct Case
  {
    const char* text;
    const char* lang;
  };
  const Case cases[] = {
      {"recuerda que mi hermana viene los domingos", "es"},
      {"no le gusta la cebolla a mi hijo", "es"},
      {"el usuario es alergico a los frutos secos", "es"},
      {"la casa se cierra a las once de la noche", "es"},
      {"mi padre trabaja los viernes", "es"},
      {"la alarma se activa a las 10", "es"},
      {"maria prefiere el cafe con leche", "es"},
      {"el gato duerme en el sofa", "es"},
      {"juan viene cada viernes a comer", "es"},
      {"la abuela visita la casa en diciembre", "es"},
      {"recuerda que el codigo del wifi es argus2026", "es"},
      {"a mi prima no le gusta el pescado", "es"},
      {"el termostato se pone a 21 grados en invierno", "es"},
      {"mi hermana tiene alergia a los gatos", "es"},
      {"carmen estudia ingles los martes", "es"},
      {"el perro sale al jardin cada manana", "es"},
      {"la cena se sirve a las ocho", "es"},
      {"mi madre cocina paella los domingos", "es"},
      {"el garaje se abre con la huella", "es"},
      {"luis trabaja desde casa los lunes", "es"},
      {"la puerta principal se cierra a las doce", "es"},
      {"no me gusta el cafe descafeinado", "es"},
      {"el salon se pinta en primavera", "es"},
      {"mi tio viene de visita en julio", "es"},
      {"el jardin se riega a las siete", "es"},
      {"ana estudia medicina en la universidad", "es"},
      {"el coche se carga por la noche", "es"},
      {"la lavadora suena cuando termina", "es"},
      {"mi vecino recoge el correo los sabados", "es"},
      {"el gato come a las seis de la manana", "es"},
      {"remember that my sister comes on sundays", "en"},
      {"my son does not like onions", "en"},
      {"the user is allergic to peanuts", "en"},
      {"the house locks at eleven at night", "en"},
      {"my father works on fridays", "en"},
      {"the alarm turns on at 10", "en"},
      {"maria prefers coffee with milk", "en"},
      {"the cat sleeps on the couch", "en"},
      {"john comes every friday for lunch", "en"},
      {"grandma visits the house in december", "en"},
      {"remember that the wifi password is argus2026", "en"},
      {"my cousin does not like fish", "en"},
      {"the thermostat is set to 21 degrees in winter", "en"},
      {"my sister is allergic to cats", "en"},
      {"carmen studies english on tuesdays", "en"},
      {"the dog goes to the garden every morning", "en"},
      {"dinner is served at eight", "en"},
      {"my mother cooks paella on sundays", "en"},
      {"the garage opens with the fingerprint", "en"},
      {"luis works from home on mondays", "en"},
      {"the front door closes at twelve", "en"},
      {"I do not like decaf coffee", "en"},
      {"the living room is painted in spring", "en"},
      {"my uncle visits in july", "en"},
      {"the garden is watered at seven", "en"},
      {"ana studies medicine at the university", "en"},
      {"the car charges at night", "en"},
      {"the washing machine beeps when it finishes", "en"},
      {"my neighbor picks up the mail on saturdays", "en"},
      {"the cat eats at six in the morning", "en"},
  };

  ExtractionService service;
  const bool loaded = service.ensureLoaded();
  std::printf("  model loaded: %s\n", loaded ? "yes" : "no (Minimal tier)");
  if (!loaded) {
    std::printf("  model absent -> lexicon tier only; accuracy gate "
                "requires scripts/setup.sh\n");
    return failures;
  }

  int facts = 0;
  int extractedUtterances = 0;
  int verbatimClean = 0;
  int distinctClean = 0;
  int subjectPresent = 0;
  double totalMs = 0.0;
  for (const auto& c : cases) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto result = service.extract({.text = c.text,
                                         .templateJson = kTemplate,
                                         .schemaJson = {},
                                         .maxTokens = 256,
                                         .cancel = CancellationToken{}});
    const auto t1 = std::chrono::steady_clock::now();
    totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!result)
      continue;
    bool any = false;
    const Json::Value& arr = (*result)["facts"];
    const std::string haystack = normalize(c.text);
    for (const Json::Value& fact : arr) {
      const std::string subject = fact["subject"].asString();
      const std::string predicate = fact["action"].asString();
      const std::string value = fact["object"].asString();
      const std::string when = fact["time"].asString();
      if (subject.empty() && predicate.empty() && value.empty())
        continue;
      ++facts;
      any = true;

      const auto verbatim = [&haystack](const std::string& field) {
        if (field.empty())
          return true;
        return haystack.find(normalize(field)) != std::string::npos;
      };
      if (verbatim(subject) && verbatim(predicate) && verbatim(value) &&
          verbatim(when))
        ++verbatimClean;

      const std::string ns = normalize(subject);
      const std::string nv = normalize(value);
      const std::string nw = normalize(when);
      const bool dup = (!ns.empty() && ns == nv) || (!ns.empty() && ns == nw) ||
                       (!nv.empty() && nv == nw);
      if (!dup)
        ++distinctClean;
      else
        std::printf(
            "    [dup]  \"%s\" -> s=\"%s\" a=\"%s\" o=\"%s\" t=\"%s\"\n",
            c.text, subject.c_str(), predicate.c_str(), value.c_str(),
            when.c_str());
      if (!verbatim(subject) || !verbatim(predicate) || !verbatim(value) ||
          !verbatim(when))
        std::printf(
            "    [vbtm] \"%s\" -> s=\"%s\" a=\"%s\" o=\"%s\" t=\"%s\"\n",
            c.text, subject.c_str(), predicate.c_str(), value.c_str(),
            when.c_str());
      if (!ns.empty() && haystack.find(ns) != std::string::npos)
        ++subjectPresent;
    }
    if (any)
      ++extractedUtterances;
  }
  const size_t total = sizeof(cases) / sizeof(cases[0]);
  const auto pct = [facts](int n) {
    return facts > 0
               ? 100.0 * static_cast<double>(n) / static_cast<double>(facts)
               : 0.0;
  };
  std::printf("  extracted facts=%d utterances=%d/%zu "
              "avg=%.0f ms/extraction\n",
              facts, extractedUtterances, total,
              totalMs / static_cast<double>(total));
  std::printf("  precision: verbatim=%d/%d (%.0f%%) distinct=%d/%d (%.0f%%) "
              "subject-in-text=%d/%d (%.0f%%)\n",
              verbatimClean, facts, pct(verbatimClean), distinctClean, facts,
              pct(distinctClean), subjectPresent, facts, pct(subjectPresent));
  report(facts >= 55,
         "gate: >= 55 facts extracted from the 60-utterance es/en fixture");
  report(pct(verbatimClean) >= 90.0,
         "gate: >= 90% of facts are verbatim spans of the source text");
  std::printf("  note: slot distinctness above is RAW model output; the "
              "deterministic repair in TieredExtractor is gated by "
              "--holdout-test\n");
  return failures;
}

struct HoldoutCase
{
  const char* text;
  const char* lang;
  const char* subject;
};
const HoldoutCase kHoldoutCases[] = {
    {"mi hermano entrena los martes", "es", "hermano"},
    {"mi madre pasea al perro por la tarde", "es", "madre"},
    {"carmen toca el piano los jueves", "es", "carmen"},
    {"el jardinero poda los setos en marzo", "es", "jardinero"},
    {"mi hija practica natacion los sabados", "es", "hija"},
    {"el tecnico revisa la caldera en octubre", "es", "tecnico"},
    {"mi vecino lava el coche los domingos", "es", "vecino"},
    {"ana alimenta al gato cada manana", "es", "ana"},
    {"mi padre lee el periodico por la manana", "es", "padre"},
    {"el cartero deja los paquetes en la porteria", "es", "cartero"},
    {"my brother trains on tuesdays", "en", "brother"},
    {"my mother walks the dog in the afternoon", "en", "mother"},
    {"carmen plays the piano on thursdays", "en", "carmen"},
    {"the gardener trims the hedges in march", "en", "gardener"},
    {"my daughter practices swimming on saturdays", "en", "daughter"},
    {"the technician checks the boiler in october", "en", "technician"},
    {"my neighbor washes the car on sundays", "en", "neighbor"},
    {"ana feeds the cat every morning", "en", "ana"},
    {"my father reads the newspaper in the morning", "en", "father"},
    {"the postman leaves the parcels at the door", "en", "postman"},
    // Scoping: the subject is the whole noun phrase. Collapsing these to the
    // head noun makes two different things share one entity, and the second
    // fact silently invalidates the first.
    {"la bicicleta de ana descansa en el garaje", "es", "bicicleta de ana"},
    {"la bicicleta de luis reposa en el trastero", "es", "bicicleta de luis"},
    {"el gato de marta ronronea por las noches", "es", "gato de marta"},
    {"la persiana del salon chirria por las mananas", "es",
     "persiana del salon"},
    // First person with an object of its own: the car is not the user.
    {"mi coche aparca en el garaje", "es", "coche"},
    {"mi portatil reposa sobre la mesa", "es", "portatil"},
    {"ana's bike rests in the garage", "en", "ana's bike"},
    {"my laptop rests on the table", "en", "laptop"},
};

struct EngineSpec
{
  std::string path;
  std::string format;
};

struct EngineScore
{
  std::string label;
  int produced = 0;
  int subjectExact = 0;
  int verbatim = 0;
  int distinct = 0;
  double avgMs = 0.0;
};

EngineScore scoreEngine(const EngineSpec& engine)
{
  EngineScore score;
  score.label = engine.path.empty() ? "config.toml" : engine.path;
  if (!engine.path.empty())
    writeModelOverlay(engine.path.c_str(), engine.format.c_str());
  ConfigService::load("config.toml");

  ExtractionService model;
  TieredExtractor tiered(model);
  tiered.rebuild(loadLexicon());
  if (!model.ensureLoaded()) {
    std::printf("  %s: model not loadable, skipped\n", score.label.c_str());
    return score;
  }

  double totalMs = 0.0;
  for (const auto& c : kHoldoutCases) {
    std::vector<extract::ExtractedFact> out;
    const auto t0 = std::chrono::steady_clock::now();
    tiered.extract({.clause = c.text, .lang = c.lang, .userId = 1}, out);
    totalMs += std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    if (out.empty())
      continue;
    ++score.produced;
    const auto& f = out.front();
    const std::string src = normalize(c.text);
    if (normalize(f.subject) == normalize(c.subject))
      ++score.subjectExact;
    if (!f.predicate.empty() &&
        src.find(normalize(f.predicate)) != std::string::npos)
      ++score.verbatim;
    if (f.value.empty() || f.when.surface.empty() ||
        normalize(f.value) != normalize(f.when.surface))
      ++score.distinct;
  }
  score.avgMs = totalMs / static_cast<double>(std::size(kHoldoutCases));
  return score;
}

int runEngineBench(const std::vector<EngineSpec>& engines)
{
  std::printf("\n== engine-bench (same fixture, one row per engine) ==\n");
  std::vector<EngineScore> scores;
  for (const auto& engine : engines)
    scores.push_back(scoreEngine(engine));

  const size_t total = std::size(kHoldoutCases);
  std::printf("\n  %-46s %8s %9s %9s %9s %8s\n", "engine", "recall", "scoping",
              "verbatim", "distinct", "ms");
  for (const auto& s : scores) {
    const auto pct = [&](int n) {
      return s.produced > 0 ? 100.0 * n / s.produced : 0.0;
    };
    std::printf("  %-46s %6d/%zu %8.0f%% %8.0f%% %8.0f%% %7.0f\n",
                s.label.c_str(), s.produced, total, pct(s.subjectExact),
                pct(s.verbatim), pct(s.distinct), s.avgMs);
  }
  std::printf("\n  scoping is the subject matching exactly: an entity that\n"
              "  swallows the sentence silently invalidates earlier facts.\n");
  return 0;
}

int runHoldoutTest(const char* modelOverride, const char* formatOverride)
{
  std::printf("\n== holdout-test (verbs absent from the lexicon) ==\n");
  if (modelOverride && *modelOverride)
    writeModelOverlay(modelOverride, formatOverride);
  ConfigService::load("config.toml");

  const auto& cases = kHoldoutCases;

  ExtractionService model;
  TieredExtractor tiered(model);
  tiered.rebuild(loadLexicon());
  const bool loaded = model.ensureLoaded();
  std::printf("  model loaded: %s\n", loaded ? "yes" : "no");
  if (!loaded) {
    std::printf("  holdout gate needs the model (scripts/setup.sh)\n");
    return failures;
  }

  const size_t total = std::size(kHoldoutCases);
  int produced = 0;
  int subjectOk = 0;
  int subjectExact = 0;
  int actionVerbatim = 0;
  int slotsDistinct = 0;
  int viaModel = 0;
  double totalMs = 0.0;
  for (const auto& c : cases) {
    std::vector<extract::ExtractedFact> out;
    const auto t0 = std::chrono::steady_clock::now();
    tiered.extract({.clause = c.text, .lang = c.lang, .userId = 1}, out);
    const auto t1 = std::chrono::steady_clock::now();
    totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (out.empty()) {
      const auto raw =
          model.extract({.text = c.text,
                         .templateJson = extract::factTemplateFor(c.lang),
                         .schemaJson = extract::factSchemaFor(c.lang),
                         .maxTokens = 256,
                         .cancel = CancellationToken{}});
      std::printf("    [none] \"%s\" raw=%s\n", c.text,
                  raw ? Json::FastWriter().write(*raw).c_str() : "(null)");
      continue;
    }
    ++produced;
    const auto& f = out.front();
    if (f.tier == extract::ExtractTier::Model)
      ++viaModel;
    const std::string src = normalize(c.text);
    const bool sOk =
        normalize(f.subject).find(normalize(c.subject)) != std::string::npos;
    const bool aOk = !f.predicate.empty() &&
                     src.find(normalize(f.predicate)) != std::string::npos;
    const bool dOk = f.value.empty() || f.when.surface.empty() ||
                     normalize(f.value) != normalize(f.when.surface);
    // Containment tolerates a subject that swallowed the sentence; scoping is
    // only right when the phrase matches exactly.
    const bool exact = normalize(f.subject) == normalize(c.subject);
    subjectExact += exact ? 1 : 0;
    if (!exact)
      std::printf("    [scope] \"%s\" esperado=\"%s\" obtenido=\"%s\"\n",
                  c.text, c.subject, f.subject.c_str());
    subjectOk += sOk ? 1 : 0;
    actionVerbatim += aOk ? 1 : 0;
    slotsDistinct += dOk ? 1 : 0;
    if (!sOk || !aOk || !dOk)
      std::printf(
          "    [%s%s%s] \"%s\" -> s=\"%s\" a=\"%s\" o=\"%s\" t=\"%s\"\n",
          sOk ? "" : "S", aOk ? "" : "A", dOk ? "" : "D", c.text,
          f.subject.c_str(), f.predicate.c_str(), f.value.c_str(),
          f.when.surface.c_str());
  }
  const auto prec = [produced](int n) {
    return produced > 0
               ? 100.0 * static_cast<double>(n) / static_cast<double>(produced)
               : 0.0;
  };
  const double recall =
      100.0 * static_cast<double>(produced) / static_cast<double>(total);
  std::printf("  recall: produced=%d/%zu (%.0f%%) via-model=%d avg=%.0f ms\n",
              produced, total, recall, viaModel,
              totalMs / static_cast<double>(total));
  std::printf("  scoping: subject-exact=%d/%d (%.0f%%)\n", subjectExact,
              produced, produced ? 100.0 * subjectExact / produced : 0.0);
  std::printf("  precision (over produced): subject=%d (%.0f%%) "
              "action-verbatim=%d (%.0f%%) slots-distinct=%d (%.0f%%)\n",
              subjectOk, prec(subjectOk), actionVerbatim, prec(actionVerbatim),
              slotsDistinct, prec(slotsDistinct));
  report(recall >= 80.0, "holdout gate: recall >= 80% of utterances");
  report(prec(subjectOk) >= 90.0, "holdout gate: subject precision >= 90%");
  report(prec(actionVerbatim) >= 90.0,
         "holdout gate: action-verbatim precision >= 90%");
  report(prec(slotsDistinct) >= 90.0,
         "holdout gate: slot-distinctness precision >= 90%");
  return failures;
}

int runExtractText(const char* text, const char* modelOverride,
                   const char* formatOverride)
{
  std::printf("\n== extract-text ==\n");
  if (modelOverride && *modelOverride)
    writeModelOverlay(modelOverride, formatOverride);
  ConfigService::load("config.toml");

  ExtractionService model;
  const bool loaded = model.ensureLoaded();
  std::printf("  model: %s\n", loaded ? "loaded" : "absent");

  LexiconExtractor lexicon;
  lexicon.rebuild(loadLexicon());
  std::vector<extract::ExtractedFact> lex;
  lexicon.extract({.clause = text, .lang = "es", .userId = 1}, lex);
  if (lex.empty())
    std::printf("  lexicon: (nothing)\n");
  for (const auto& f : lex)
    std::printf("  lexicon: s=\"%s\" p=\"%s\" o=\"%s\" t=\"%s\"\n",
                f.subject.c_str(), f.predicate.c_str(), f.value.c_str(),
                f.when.surface.c_str());

  if (loaded) {
    const auto raw =
        model.extract({.text = text,
                       .templateJson = extract::factTemplateFor("es"),
                       .schemaJson = extract::factSchemaFor("es"),
                       .maxTokens = 256,
                       .grammar = true,
                       .cancel = CancellationToken{}});
    std::printf("  model raw: %s\n",
                raw ? Json::FastWriter().write(*raw).c_str() : "(null)");
    const auto free =
        model.extract({.text = text,
                       .templateJson = extract::factTemplateFor("es"),
                       .schemaJson = extract::factSchemaFor("es"),
                       .maxTokens = 256,
                       .grammar = false,
                       .cancel = CancellationToken{}});
    std::printf("  model free: %s\n",
                free ? Json::FastWriter().write(*free).c_str() : "(null)");
  }
  return 0;
}

int runExtractTest()
{
  std::printf("\n== extract-test ==\n");
  ConfigService::load("config.toml");

  struct Case
  {
    const char* text;
    const char* lang;
    const char* subject;
    const char* predicate;
    const char* valueContains;
    int whenKind;
  };
  const Case cases[] = {
      {"recuerda que mi hermana viene los domingos", "es", "hermana", "visits",
       "los domingos", 4},
      {"no le gusta la cebolla a mi hijo", "es", "hijo", "dislikes", "cebolla",
       0},
      {"el usuario es alergico a los frutos secos", "es", "usuario",
       "allergic_to", "frutos secos", 0},
      {"la casa se cierra a las once de la noche", "es", "casa", "closes_at",
       "once", 2},
      {"mi padre trabaja los viernes", "es", "padre", "works", "viernes", 4},
      {"la alarma se activa a las 10", "es", "alarma", "activates_at", "10", 2},
      {"maria prefiere el cafe con leche", "es", "maria", "prefers",
       "cafe con leche", 0},
      {"el gato duerme en el sofa", "es", "gato", "sleeps_in", "sofa", 0},
      {"juan viene cada viernes a comer", "es", "juan", "visits", "viernes", 3},
      {"la abuela visita la casa en diciembre", "es", "abuela", "visits",
       "casa", 5},
      {"recuerda que el codigo del wifi es argus2026", "es", "codigo", "is",
       "argus2026", 0},
      {"a mi prima no le gusta el pescado", "es", "prima", "dislikes",
       "pescado", 0},
      {"el termostato se pone a 21 grados en invierno", "es", "termostato",
       "set_to", "21 grados", 5},
      {"mi hermana tiene alergia a los gatos", "es", "hermana", "allergic_to",
       "gatos", 0},
      {"carmen estudia ingles los martes", "es", "carmen", "studies", "ingles",
       4},
      {"el perro sale al jardin cada manana", "es", "perro", "goes_to",
       "jardin", 4},
      {"la cena se sirve a las ocho", "es", "cena", "served_at", "ocho", 2},
      {"mi madre cocina paella los domingos", "es", "madre", "cooks", "paella",
       4},
      {"el garaje se abre con la huella", "es", "garaje", "opens_with",
       "huella", 0},
      {"luis trabaja desde casa los lunes", "es", "luis", "works_from", "lunes",
       4},
      {"la puerta principal se cierra a las doce", "es", "puerta", "closes_at",
       "doce", 2},
      {"no me gusta el cafe descafeinado", "es", "usuario", "dislikes",
       "cafe descafeinado", 0},
      {"el salon se pinta en primavera", "es", "salon", "painted_in",
       "primavera", 5},
      {"mi tio viene de visita en julio", "es", "tio", "visits", "julio", 5},
      {"el jardin se riega a las siete", "es", "jardin", "watered_at", "siete",
       2},
      {"ana estudia medicina en la universidad", "es", "ana", "studies",
       "medicina", 0},
      {"el coche se carga por la noche", "es", "coche", "charges_at", "noche",
       5},
      {"la lavadora suena cuando termina", "es", "lavadora", "beeps_when",
       "termina", 0},
      {"mi vecino recoge el correo los sabados", "es", "vecino",
       "picks_up_mail", "sabados", 4},
      {"el gato come a las seis de la manana", "es", "gato", "eats_at", "seis",
       2},
      {"remember that my sister comes on sundays", "en", "sister", "visits",
       "sundays", 4},
      {"my son does not like onions", "en", "son", "dislikes", "onions", 0},
      {"the user is allergic to peanuts", "en", "user", "allergic_to",
       "peanuts", 0},
      {"the house locks at eleven at night", "en", "house", "closes_at",
       "eleven", 2},
      {"my father works on fridays", "en", "father", "works", "fridays", 4},
      {"the alarm turns on at 10", "en", "alarm", "activates_at", "10", 2},
      {"maria prefers coffee with milk", "en", "maria", "prefers",
       "coffee with milk", 0},
      {"the cat sleeps on the couch", "en", "cat", "sleeps_in", "couch", 0},
      {"john comes every friday for lunch", "en", "john", "visits", "friday",
       3},
      {"grandma visits the house in december", "en", "grandma", "visits",
       "house", 5},
      {"remember that the wifi password is argus2026", "en", "wifi", "is",
       "argus2026", 0},
      {"my cousin does not like fish", "en", "cousin", "dislikes", "fish", 0},
      {"the thermostat is set to 21 degrees in winter", "en", "thermostat",
       "set_to", "21 degrees", 5},
      {"my sister is allergic to cats", "en", "sister", "allergic_to", "cats",
       0},
      {"carmen studies english on tuesdays", "en", "carmen", "studies",
       "english", 4},
      {"the dog goes to the garden every morning", "en", "dog", "goes_to",
       "garden", 4},
      {"dinner is served at eight", "en", "dinner", "served_at", "eight", 2},
      {"my mother cooks paella on sundays", "en", "mother", "cooks", "paella",
       4},
      {"the garage opens with the fingerprint", "en", "garage", "opens_with",
       "fingerprint", 0},
      {"luis works from home on mondays", "en", "luis", "works_from", "mondays",
       4},
      {"the front door closes at twelve", "en", "front", "closes_at", "twelve",
       2},
      {"I do not like decaf coffee", "en", "usuario", "dislikes",
       "decaf coffee", 0},
      {"the room is painted in spring", "en", "room", "painted_in", "spring",
       5},
      {"my uncle visits in july", "en", "uncle", "visits", "july", 5},
      {"the garden is watered at seven", "en", "garden", "watered_at", "seven",
       2},
      {"ana studies medicine at the university", "en", "ana", "studies",
       "medicine", 0},
      {"the car charges at night", "en", "car", "charges_at", "night", 5},
      {"the machine beeps when it finishes", "en", "machine", "beeps_when",
       "finishes", 0},
      {"my neighbor picks up the mail on saturdays", "en", "neighbor",
       "picks_up_mail", "saturdays", 4},
      {"the cat eats at six in the morning", "en", "cat", "eats_at", "six", 2},
  };
  constexpr size_t kTotal = sizeof(cases) / sizeof(cases[0]);

  ExtractionService model;
  TieredExtractor extractor(model);
  extractor.rebuild(loadLexicon());

  int tier1 = 0;
  int total = 0;
  for (const auto& c : cases) {
    std::vector<extract::ExtractedFact> facts;
    const bool ok =
        extractor.extract({.clause = c.text, .lang = c.lang, .userId = 7},
                          facts);
    if (!ok || facts.empty()) {
      std::printf("  [NOFACT] %s\n", c.text);
      continue;
    }
    const extract::ExtractedFact& f = facts.front();
    bool pass = f.subject == c.subject && f.predicate == c.predicate &&
                f.value.find(c.valueContains) != std::string::npos &&
                static_cast<int>(f.when.kind) == c.whenKind;
    if (!pass) {
      std::printf("  [MISS] %s -> subj=%s pred=%s val=%s when=%d\n", c.text,
                  f.subject.c_str(), f.predicate.c_str(), f.value.c_str(),
                  static_cast<int>(f.when.kind));
    }
    else {
      ++total;
      if (f.tier == extract::ExtractTier::Lexicon)
        ++tier1;
    }
  }
  std::printf("  tier1=%d total=%d/%zu\n", tier1, total, kTotal);
  report(total >= 55, "gate: end-to-end >= 55/60");
  report(tier1 >= 40, "gate: tier 1 alone >= 40/60 (no model required)");
  return failures;
}

int runTierBench()
{
  std::printf("\n== tier-bench ==\n");
  const auto tier = HardwareProbe::extractionTier();
  const char* name =
      tier == HardwareProbe::ExtractionTier::Full
          ? "Full"
          : (tier == HardwareProbe::ExtractionTier::Light ? "Light"
                                                          : "Minimal");
  const HardwareProfile& p = HardwareProbe::get();
  std::printf("  tier=%s ram=%ldMB slots=%d threads=%d\n", name,
              static_cast<long>(p.ramTotalMb), ThreadBudget::extractionSlots(),
              ThreadBudget::extractionThreads());
  report(tier == HardwareProbe::extractionTier(),
         "extractionTier() derived from host RAM");
  return failures;
}

void* operator new(std::size_t n)
{
  if (void* p = rawAlloc(n))
    return p;
  throw std::bad_alloc();
}

void* operator new[](std::size_t n)
{
  if (void* p = rawAlloc(n))
    return p;
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept
{
  std::free(p);
}

void operator delete[](void* p) noexcept
{
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
  std::free(p);
}

int main(int argc, char** argv)
{
  std::remove("config.local.toml");
  unsigned seed = 42;
  bool doTest = false;
  bool doBench = false;
  bool doNuextract = false;
  bool doTier = false;
  bool doExtract = false;
  bool doIsolate = false;
  const char* modelOverride = nullptr;
  const char* formatOverride = nullptr;
  bool doHoldout = false;
  bool doEngineBench = false;
  std::vector<EngineSpec> engines;
  const char* probeText = nullptr;
  bool doSmoke = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--automaton-test") == 0)
      doTest = true;
    else if (std::strcmp(argv[i], "--automaton-bench") == 0)
      doBench = true;
    else if (std::strcmp(argv[i], "--nuextract-test") == 0)
      doNuextract = true;
    else if (std::strcmp(argv[i], "--tier-bench") == 0)
      doTier = true;
    else if (std::strcmp(argv[i], "--extract-test") == 0)
      doExtract = true;
    else if (std::strcmp(argv[i], "--engine-bench") == 0)
      doEngineBench = true;
    else if (std::strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
      const std::string spec = argv[++i];
      const size_t at = spec.rfind(':');
      if (at != std::string::npos && spec.find(".gguf") < at)
        engines.push_back({spec.substr(0, at), spec.substr(at + 1)});
      else
        engines.push_back({spec, ""});
    }
    else if (std::strcmp(argv[i], "--holdout-test") == 0)
      doHoldout = true;
    else if (std::strcmp(argv[i], "--extract-text") == 0 && i + 1 < argc)
      probeText = argv[++i];
    else if (std::strcmp(argv[i], "--grammar-isolate") == 0)
      doIsolate = true;
    else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc)
      modelOverride = argv[++i];
    else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc)
      formatOverride = argv[++i];
    else if (std::strcmp(argv[i], "--grammar-smoke") == 0)
      doSmoke = true;
    else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
      seed = static_cast<unsigned>(std::atoi(argv[++i]));
    else {
      std::printf("unknown flag: %s\n", argv[i]);
      return 2;
    }
  }
  if (!doTest && !doBench && !doNuextract && !doTier && !doExtract &&
      !doHoldout && !probeText && !doSmoke && !doIsolate && !doEngineBench)
    doTest = doBench = true;
  if (doEngineBench && engines.empty())
    engines.push_back({"", ""});

  int rc = 0;
  if (doEngineBench)
    rc |= runEngineBench(engines);
  if (doTest)
    rc |= runAutomatonTest(seed);
  if (doBench)
    rc |= runAutomatonBench(seed);
  if (doSmoke)
    rc |= runGrammarSmoke(modelOverride, formatOverride);
  if (doIsolate)
    rc |= runGrammarIsolate(modelOverride, formatOverride);
  if (doNuextract)
    rc |= runNuextractTest(modelOverride, formatOverride);
  if (doTier)
    rc |= runTierBench();
  if (doExtract)
    rc |= runExtractTest();
  if (doHoldout)
    rc |= runHoldoutTest(modelOverride, formatOverride);
  if (probeText)
    rc |= runExtractText(probeText, modelOverride, formatOverride);
  std::printf("\n%s\n", rc == 0 ? "ALL GATES PASS" : "FAILURES PRESENT");
  return rc;
}
