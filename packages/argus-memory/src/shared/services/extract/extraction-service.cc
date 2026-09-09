#include "extraction-service.hxx"

#include <algorithm>
#include <array>
#include <drogon/drogon.h>
#include <llama.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/extract/extract-contracts.hxx>
#include <shared/wrapper/ai-init/ai-init.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <utility>

namespace
{

bool hasFile(const std::string& path)
{
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  std::fclose(f);
  return true;
}

std::string gbnfEscape(const std::string& s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

struct ResolvedTemplate
{
  Json::Value parsed;
  std::string raw;
};

std::vector<std::string> orderedMembers(const Json::Value& node,
                                        const std::string& raw)
{
  std::vector<std::string> members = node.getMemberNames();
  std::stable_sort(members.begin(), members.end(),
                   [&raw](const std::string& a, const std::string& b) {
                     return raw.find("\"" + a + "\"") <
                            raw.find("\"" + b + "\"");
                   });
  return members;
}

std::string canonicalTemplate(const Json::Value& node, const std::string& raw)
{
  if (node.isObject()) {
    std::string out = "{";
    const std::vector<std::string> members = orderedMembers(node, raw);
    for (size_t i = 0; i < members.size(); ++i) {
      if (i > 0)
        out += ", ";
      out +=
          "\"" + members[i] + "\": " + canonicalTemplate(node[members[i]], raw);
    }
    return out + "}";
  }
  if (node.isArray()) {
    if (node.empty())
      return "[]";
    return "[" + canonicalTemplate(node[0], raw) + "]";
  }
  return "\"\"";
}

ResolvedTemplate resolveTemplate(const std::string& requested)
{
  Json::Value parsed;
  if (!requested.empty() && Json::Reader().parse(requested, parsed) &&
      (parsed.isObject() || parsed.isArray()))
    return {parsed, requested};
  Json::Value fallback;
  Json::Reader().parse(extract::kFactTemplateEs, fallback);
  return {fallback, extract::kFactTemplateEs};
}

} // namespace

void ExtractionService::llamaModelFree(llama_model* m)
{
  if (m)
    llama_model_free(m);
}

void ExtractionService::llamaFree(llama_context* c)
{
  if (c)
    llama_free(c);
}

ExtractionService::ExtractionService() = default;

ExtractionService::~ExtractionService() = default;

void ExtractionService::ContextSlot::llamaSamplerFree(llama_sampler* s)
{
  if (s)
    llama_sampler_free(s);
}

ExtractPromptFormat extractPromptFormatFromString(const std::string& raw)
{
  if (raw == "v2")
    return ExtractPromptFormat::V2;
  if (raw == "lfm")
    return ExtractPromptFormat::Lfm;
  return ExtractPromptFormat::V15;
}

std::string ExtractionService::buildPrompt(const PromptInput& input)
{
  if (input.format == ExtractPromptFormat::Lfm) {
    return "<|im_start|>system\nReturn data as a JSON object with the "
           "following schema:\n" +
           (input.schemaJson.empty() ? input.templateJson : input.schemaJson) +
           "<|im_end|>\n<|im_start|>user\n" + input.text +
           "<|im_end|>\n<|im_start|>assistant\n";
  }
  if (input.format == ExtractPromptFormat::V2) {
    return "<|im_start|>system\nYou are NuExtract, an information extraction "
           "tool created by NuMind.<|im_end|><|im_start|>user\n# Template:\n" +
           input.templateJson + "\n# Context:\n" + input.text +
           "<|im_end|>\n<|im_start|>assistant";
  }
  return "<|input|>\n### Template:\n" + input.templateJson + "\n### Text:\n" +
         input.text + "\n\n<|output|>\n";
}

std::string ExtractionService::buildGrammar(const Json::Value& templateJson,
                                            const std::string& rawOrder)
{
  std::vector<std::string> rules;
  const auto leafString = [&](std::vector<std::string>& rulesOut) {
    rulesOut.push_back("str ::= \"\\\"\" ([^\"\\\\] | \"\\\\\" .)* \"\\\"\"\n");
  };
  const auto build = [&](const auto& self, const Json::Value& node,
                         const std::string& name) -> void {
    if (node.isObject()) {
      std::string body = "\"{\" ws ";
      const std::vector<std::string> members = orderedMembers(node, rawOrder);
      for (size_t i = 0; i < members.size(); ++i) {
        const std::string childName = name + "-p" + std::to_string(i);
        self(self, node[members[i]], childName);
        body += "\"\\\"" + gbnfEscape(members[i]) + "\\\"\" ws \":\" ws " +
                childName + " ws";
        if (i + 1 < members.size())
          body += "\",\" ws ";
      }
      body += "\"}\"";
      rules.push_back(name + " ::= " + body + "\n");
      return;
    }
    if (node.isArray()) {
      if (node.size() == 0) {
        rules.push_back(name + " ::= \"[\" ws (str (\",\" ws str)*)? "
                               "ws \"]\"\n");
        return;
      }
      const std::string childName = name + "-e";
      self(self, node[0], childName);
      rules.push_back(name + " ::= \"[\" ws (" + childName + " (\",\" ws " +
                      childName + ")*)? ws \"]\"\n");
      return;
    }
    rules.push_back(name + " ::= str\n");
  };
  rules.push_back("root ::= ws " + std::string("r0") + " ws\n");
  build(build, templateJson, "r0");
  rules.push_back("ws ::= \" \"?\n");
  leafString(rules);
  std::string grammar;
  for (const auto& rule : rules)
    grammar += rule;
  return grammar;
}

bool ExtractionService::ensureLoaded()
{
  std::scoped_lock lock(loadMutex_);
  if (loaded_)
    return true;
  return loadLocked();
}

bool ExtractionService::loadLocked()
{
  std::lock_guard<std::mutex> lock(ai_init::llamaMutex());

  const std::string modelPath = ConfigService::getString("extract.model_path");
  if (modelPath.empty() || !hasFile(modelPath)) {
    LOG_INFO << "ExtractionService: model absent (" << modelPath
             << ") — Minimal tier, lexicon only";
    return false;
  }

  const int gpuLayers = [&] {
    const int cfg = ConfigService::getInt("extract.gpu_layers");
    if (cfg >= 0)
      return cfg;
    return HardwareProbe::llmGpuLayers();
  }();

  llama_model_params modelParams = llama_model_default_params();
  modelParams.n_gpu_layers = gpuLayers;
  modelParams.load_mode = LLAMA_LOAD_MODE_MMAP;

  llama_model* rawModel =
      llama_model_load_from_file(modelPath.c_str(), modelParams);
  if (!rawModel) {
    LOG_ERROR << "ExtractionService: model load failed: " << modelPath;
    return false;
  }
  model_.reset(rawModel);

  nCtx_ = std::max(256, ConfigService::getInt("extract.n_ctx"));
  maxTokens_ = std::max(16, ConfigService::getInt("extract.max_tokens"));
  const int threadsCfg = ConfigService::getInt("extract.threads");
  threads_ = threadsCfg > 0 ? threadsCfg : ThreadBudget::extractionThreads();

  format_ = extractPromptFormatFromString(
      ConfigService::getString("extract.prompt_format"));

  const int slots = ThreadBudget::extractionSlots();

  slots_.clear();
  slots_.reserve(slots);
  for (int i = 0; i < slots; ++i) {
    auto slot = std::make_unique<ContextSlot>();
    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx = static_cast<uint32_t>(nCtx_);
    ctxParams.n_batch = 256;
    ctxParams.n_ubatch = 256;
    ctxParams.n_threads = threads_;
    ctxParams.n_threads_batch = threads_;
    ctxParams.no_perf = true;
    ctxParams.offload_kqv = gpuLayers > 0;
    llama_context* rawCtx = llama_init_from_model(model_.get(), ctxParams);
    if (!rawCtx) {
      LOG_ERROR << "ExtractionService: context " << i << " failed";
      slots_.clear();
      model_.reset();
      return false;
    }
    slot->ctx.reset(rawCtx);
    slot->batch = std::make_unique<llama_batch>(llama_batch_init(256, 0, 1));
    slot->genBatch = std::make_unique<llama_batch>(llama_batch_init(1, 0, 1));

    slots_.push_back(std::move(slot));
  }

  permits_.release(slots);
  loaded_ = true;
  LOG_INFO << "ExtractionService: loaded " << modelPath << " slots=" << slots
           << " threads=" << threads_ << " n_ctx=" << nCtx_
           << " gpu_layers=" << gpuLayers;
  return true;
}

void ExtractionService::unloadIfIdle()
{
  std::scoped_lock lock(loadMutex_);
  if (!loaded_)
    return;
  int acquired = 0;
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    if (permits_.try_acquire())
      ++acquired;
  }
  if (acquired != static_cast<int>(slots_.size())) {
    permits_.release(acquired);
    return;
  }
  slots_.clear();
  model_.reset();
  loaded_ = false;
  LOG_INFO << "ExtractionService: unloaded after idle";
}

std::optional<Json::Value>
ExtractionService::extract(const ExtractRequest& request)
{
  if (!ensureLoaded())
    return std::nullopt;

  permits_.acquire();
  ContextSlot* slot = nullptr;
  for (auto& candidate : slots_) {
    if (candidate->mtx.try_lock()) {
      slot = candidate.get();
      break;
    }
  }
  if (!slot) {
    permits_.release();
    return std::nullopt;
  }
  const auto result = extractOnSlot(*slot, request);
  slot->mtx.unlock();
  permits_.release();
  return result;
}

std::string ExtractionService::grammarFor(const GrammarInput& input)
{
  std::scoped_lock lock(grammarMutex_);
  if (grammarKey_ != input.canonicalTemplate) {
    grammarValue_ =
        buildGrammar(input.parsedTemplate, input.rawOrder);
    grammarKey_ = input.canonicalTemplate;
  }
  return grammarValue_;
}

std::optional<Json::Value>
ExtractionService::extractOnSlot(ContextSlot& slot,
                                 const ExtractRequest& request)
{
  if (auto mem = llama_get_memory(slot.ctx.get()))
    llama_memory_clear(mem, true);

  const ResolvedTemplate tpl = resolveTemplate(request.templateJson);
  const std::string canonical = canonicalTemplate(tpl.parsed, tpl.raw);
  const std::string grammar =
      grammarFor({.canonicalTemplate = canonical,
                  .parsedTemplate = tpl.parsed,
                  .rawOrder = tpl.raw});

  const auto* vocab = llama_model_get_vocab(model_.get());
  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  std::unique_ptr<llama_sampler, void (*)(llama_sampler*)>
      sampler(llama_sampler_chain_init(sparams),
              &ContextSlot::llamaSamplerFree);
  llama_sampler* grammarSampler =
      request.grammar
          ? llama_sampler_init_grammar(vocab, grammar.c_str(), "root")
          : nullptr;
  if (!sampler || (request.grammar && !grammarSampler)) {
    LOG_ERROR << "ExtractionService: grammar parse failed";
    return std::nullopt;
  }
  if (grammarSampler)
    llama_sampler_chain_add(sampler.get(), grammarSampler);
  llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());

  const std::string prompt = buildPrompt({.templateJson = canonical,
                                          .schemaJson = request.schemaJson,
                                          .text = request.text,
                                          .format = format_});
  const std::vector<llama_token> tokens = [&] {
    const int32_t probe = llama_tokenize(vocab, prompt.c_str(),
                                         static_cast<int32_t>(prompt.size()),
                                         nullptr, 0, true, true);
    const size_t needed =
        probe < 0 ? static_cast<size_t>(-probe) : static_cast<size_t>(probe);
    std::vector<llama_token> out(needed);
    if (needed > 0) {
      llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                     out.data(), static_cast<int32_t>(needed), true, true);
    }
    return out;
  }();

  auto& batch = *slot.batch;
  const size_t total = tokens.size();
  for (size_t start = 0; start < total; start += 256) {
    const size_t count = std::min(static_cast<size_t>(256), total - start);
    const bool lastChunk = (start + count) >= total;
    for (size_t j = 0; j < count; ++j) {
      const size_t pos = start + j;
      batch.token[j] = tokens[pos];
      batch.pos[j] = static_cast<int32_t>(pos);
      batch.n_seq_id[j] = 1;
      batch.seq_id[j][0] = 0;
      batch.logits[j] = (lastChunk && j == count - 1) ? 1 : 0;
    }
    batch.n_tokens = static_cast<int32_t>(count);
    if (llama_decode(slot.ctx.get(), batch) != 0)
      return std::nullopt;
  }

  const llama_token eos = llama_vocab_eos(vocab);
  const llama_token eot = llama_vocab_eot(vocab);
  llama_pos pos = static_cast<llama_pos>(total);
  std::string output;
  try {
    for (int32_t i = 0; i < request.maxTokens; ++i) {
      if (request.cancel.cancelled())
        return std::nullopt;
      const llama_token token =
          llama_sampler_sample(sampler.get(), slot.ctx.get(), -1);
      if (token == eos || token == eot)
        break;
      std::array<char, 256> buf;
      const int n = llama_token_to_piece(vocab, token, buf.data(),
                                         buf.size(), 0, true);
      if (n > 0)
        output.append(buf.data(), static_cast<size_t>(n));

      auto& gen = *slot.genBatch;
      gen.token[0] = token;
      gen.pos[0] = pos++;
      gen.n_seq_id[0] = 1;
      gen.seq_id[0][0] = 0;
      gen.logits[0] = 1;
      gen.n_tokens = 1;
      if (llama_decode(slot.ctx.get(), gen) != 0)
        break;
    }
  }
  catch (const std::exception&) {
    return std::nullopt;
  }

  Json::Value root;
  Json::Reader reader;
  if (!reader.parse(output, root)) {
    LOG_WARN << "ExtractionService: unparsable output (" << output.size()
             << " bytes): " << output;
    return std::nullopt;
  }
  if (!root.isObject() && !root.isArray()) {
    LOG_WARN << "ExtractionService: bad shape: " << output;
    return std::nullopt;
  }
  return root;
}

drogon::Task<std::optional<Json::Value>>
ExtractionService::extractAsync(const ExtractRequest& request)
{
  co_return co_await BlockingTask<std::optional<Json::Value>>{
      [this, request] { return extract(request); }};
}
