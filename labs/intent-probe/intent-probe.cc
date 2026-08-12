#include <algorithm>
#include <args.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fasttext.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/intent/intent-service.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr const char* kLabelPrefix = "__label__";
constexpr const char* kLabelCamera = "camera";
constexpr const char* kLabelMemorySave = "memory_save";
constexpr const char* kLabelNone = "none";

int fails = 0;

struct Case
{
  std::string label;
  std::string text;
};

std::vector<std::string> readLines(const std::string& path)
{
  std::vector<std::string> lines;
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cout << "[FAIL] cannot open " << path << "\n";
    ++fails;
    return lines;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty())
      lines.push_back(line);
  }
  return lines;
}

std::vector<Case> readTsv(const std::string& path)
{
  std::vector<Case> cases;
  for (const auto& line : readLines(path)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    cases.push_back(
        {.label = line.substr(0, tab), .text = line.substr(tab + 1)});
  }
  return cases;
}

std::string stripLabel(const std::string& label)
{
  if (label.rfind(kLabelPrefix, 0) == 0)
    return label.substr(std::string(kLabelPrefix).size());
  return label;
}

std::map<std::string, float> predictScores(fasttext::FastText& model,
                                           const std::string& text)
{
  std::map<std::string, float> scores;
  if (!IntentService::isMatchable(text))
    return scores;
  std::vector<std::pair<fasttext::real, std::string>> predictions;
  std::istringstream stream(IntentService::normalize(text));
  model.predictLine(stream, predictions, -1, 0.0F);
  for (const auto& [score, label] : predictions)
    scores[stripLabel(label)] = score;
  return scores;
}

std::vector<std::string> modelLabels(const fasttext::FastText& model)
{
  std::vector<std::string> labels;
  const auto dict = model.getDictionary();
  if (!dict)
    return labels;
  labels.reserve(static_cast<size_t>(dict->nlabels()));
  for (int32_t i = 0; i < dict->nlabels(); ++i)
    labels.push_back(dict->getLabel(i));
  return labels;
}

struct TrainOptions
{
  std::string dataDir;
  std::string outputBase;
  bool includeUsage;
  int epoch;
  double lr;
  fasttext::loss_name loss;
  int dim;
  int bucket;
  int minn;
  int maxn;
};

int trainImpl(const TrainOptions& opts)
{
  ConfigService::load("config.toml");

  const std::string trainPath = opts.dataDir + "/train.tsv";
  const std::string usagePath = opts.dataDir + "/usage.tsv";
  std::string input = trainPath;

  if (opts.includeUsage) {
    const auto usage = readTsv(usagePath);
    std::cout << "usage samples: " << usage.size() << "\n";
    if (!usage.empty()) {
      const std::string merged = opts.dataDir + "/train.merged.tsv";
      std::ofstream out(merged);
      if (out.is_open()) {
        for (const auto& line : readLines(trainPath)) {
          const size_t space = line.find(' ');
          if (space != std::string::npos)
            out << line.substr(0, space) << " "
                << IntentService::normalize(line.substr(space + 1)) << "\n";
        }
        for (const auto& c : usage) {
          if (c.label == kLabelCamera || c.label == kLabelMemorySave)
            out << kLabelPrefix << c.label << " "
                << IntentService::normalize(c.text) << "\n";
        }
        out.close();
        input = merged;
      }
    }
  }
  else {
    const std::string normalized = opts.dataDir + "/train.norm.tsv";
    std::ofstream out(normalized);
    if (out.is_open()) {
      for (const auto& line : readLines(trainPath)) {
        const size_t space = line.find(' ');
        if (space != std::string::npos)
          out << line.substr(0, space) << " "
              << IntentService::normalize(line.substr(space + 1)) << "\n";
      }
      out.close();
      input = normalized;
    }
  }

  const auto lines = readLines(input);
  size_t camera = 0, save = 0, other = 0;
  for (const auto& line : lines) {
    const size_t space = line.find(' ');
    const std::string label =
        stripLabel(space == std::string::npos ? line : line.substr(0, space));
    if (label == kLabelCamera)
      ++camera;
    else if (label == kLabelMemorySave)
      ++save;
    else
      ++other;
  }
  std::cout << "train lines: " << lines.size() << " (camera=" << camera
            << " memory_save=" << save << " none=" << other << ")\n";

  fasttext::Args args;
  args.input = input;
  args.output = opts.outputBase;
  args.lr = opts.lr;
  args.epoch = opts.epoch;
  args.wordNgrams = 2;
  args.minn = opts.minn;
  args.maxn = opts.maxn;
  args.dim = opts.dim;
  args.bucket = opts.bucket;
  args.loss = opts.loss;
  args.model = fasttext::model_name::sup;
  args.minCount = 1;
  args.minCountLabel = 0;
  args.thread = ThreadBudget::lightThreads();
  args.label = kLabelPrefix;
  args.verbose = 2;

  fasttext::FastText model;
  const auto t0 = std::chrono::steady_clock::now();
  model.train(args);
  const std::string outPath = opts.outputBase + ".bin";
  std::filesystem::create_directories(
      std::filesystem::path(outPath).parent_path());
  model.saveModel(outPath);
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  std::cout << "trained in " << secs << "s -> " << outPath << "\n";
  return fails == 0 ? 0 : 1;
}

int quantizeImpl(const std::string& inPath, const std::string& outPath)
{
  fasttext::FastText model;
  try {
    model.loadModel(inPath);
  }
  catch (const std::exception& e) {
    std::cout << "[FAIL] load " << inPath << ": " << e.what() << "\n";
    return 1;
  }

  fasttext::Args args;
  args.qnorm = true;
  args.qout = false;
  args.dsub = 2;
  args.retrain = 1;
  args.verbose = 1;

  model.quantize(args);
  model.saveModel(outPath);

  const auto before = std::filesystem::file_size(inPath);
  const auto after = std::filesystem::file_size(outPath);
  std::cout << "quantized: " << inPath << " (" << before / 1024.0 / 1024.0
            << " MB) -> " << outPath << " (" << after / 1024.0 / 1024.0
            << " MB)\n";
  return 0;
}

struct Expected
{
  bool camera;
  bool memorySave;
};

Expected expectedFor(const std::string& label)
{
  if (label == kLabelCamera)
    return {.camera = true, .memorySave = false};
  if (label == kLabelMemorySave)
    return {.camera = false, .memorySave = true};
  return {.camera = false, .memorySave = false};
}

int checkFileImpl(const std::string& modelPath, const std::string& casesPath);

int checkImpl(const std::string& modelPath, const std::string& dataDir)
{
  return checkFileImpl(modelPath, dataDir + "/check.tsv");
}

int checkFileImpl(const std::string& modelPath, const std::string& casesPath)
{
  ConfigService::load("config.toml");

  fasttext::FastText model;
  try {
    model.loadModel(modelPath);
  }
  catch (const std::exception& e) {
    std::cout << "[FAIL] load " << modelPath << ": " << e.what() << "\n";
    return 1;
  }
  std::cout << "model: " << modelPath << " labels:";
  for (const auto& label : modelLabels(model))
    std::cout << " " << label;
  std::cout << "\n";

  const auto cases = readTsv(casesPath);
  std::cout << "check cases: " << cases.size() << "\n";

  double cfgCamera = ConfigService::getDouble("intent.camera_threshold");
  double cfgSave = ConfigService::getDouble("intent.memory_save_threshold");
  if (cfgCamera <= 0.0)
    cfgCamera = 0.5;
  if (cfgSave <= 0.0)
    cfgSave = 0.5;

  int passed = 0;
  for (const auto& c : cases) {
    const auto scores = predictScores(model, c.text);
    const float cam =
        scores.count(kLabelCamera) ? scores.at(kLabelCamera) : 0.0F;
    const float save =
        scores.count(kLabelMemorySave) ? scores.at(kLabelMemorySave) : 0.0F;
    const Expected exp = expectedFor(c.label);
    const bool camOk = (cam >= cfgCamera) == exp.camera;
    const bool saveOk = (save >= cfgSave) == exp.memorySave;
    const bool ok = camOk && saveOk;
    if (ok)
      ++passed;
    else
      ++fails;
    std::cout << (ok ? "[ok] " : "[FAIL] ")
              << (c.label == kLabelNone ? "none" : c.label) << " camera=" << cam
              << " memory_save=" << save << " | \"" << c.text << "\"\n";
  }
  std::cout << "check: " << passed << "/" << cases.size() << " passed\n";

  const auto gridFor = [&](const std::string& label,
                           const std::vector<const Case*>& positives) {
    double bestT = 0.5, bestF1 = -1.0, bestP = 0.0;
    for (int step = 5; step <= 95; step += 5) {
      const double t = step / 100.0;
      int tp = 0, fp = 0, fn = 0;
      for (const auto& c : cases) {
        const auto scores = predictScores(model, c.text);
        const float s = scores.count(label) ? scores.at(label) : 0.0F;
        const bool isPos = std::any_of(positives.begin(), positives.end(),
                                       [&](const Case* p) { return p == &c; });
        if (isPos) {
          if (s >= t)
            ++tp;
          else
            ++fn;
        }
        else if (s >= t) {
          ++fp;
        }
      }
      const double p = tp + fp > 0 ? static_cast<double>(tp) / (tp + fp) : 0.0;
      const double r = tp + fn > 0 ? static_cast<double>(tp) / (tp + fn) : 0.0;
      const double f1 = p + r > 0 ? 2.0 * p * r / (p + r) : 0.0;
      if (f1 > bestF1 || (f1 == bestF1 && p > bestP)) {
        bestF1 = f1;
        bestT = t;
        bestP = p;
      }
    }
    std::cout << "suggested " << label << "_threshold = " << bestT
              << " (F1=" << bestF1 << ")\n";
  };

  std::vector<const Case*> camPos, savePos;
  for (const auto& c : cases) {
    if (c.label == kLabelCamera)
      camPos.push_back(&c);
    else if (c.label == kLabelMemorySave)
      savePos.push_back(&c);
  }
  gridFor(kLabelCamera, camPos);
  gridFor(kLabelMemorySave, savePos);

  return fails == 0 ? 0 : 1;
}

int evalImpl(const std::string& modelPath, const std::string& validPath)
{
  fasttext::FastText model;
  try {
    model.loadModel(modelPath);
  }
  catch (const std::exception& e) {
    std::cout << "[FAIL] load " << modelPath << ": " << e.what() << "\n";
    return 1;
  }

  const auto lines = readLines(validPath);
  int correct = 0, total = 0;
  std::map<std::string, int> tp, fp, fn;
  for (const auto& line : lines) {
    const size_t space = line.find(' ');
    if (space == std::string::npos)
      continue;
    const std::string expected = stripLabel(line.substr(0, space));
    const std::string text = line.substr(space + 1);
    ++total;
    const auto scores = predictScores(model, text);
    const auto top = std::max_element(scores.begin(), scores.end(),
                                      [](const auto& a, const auto& b) {
                                        return a.second < b.second;
                                      });
    const std::string predicted = top != scores.end() ? top->first : "";
    if (predicted == expected) {
      ++correct;
      ++tp[expected];
    }
    else {
      ++fn[expected];
      if (!predicted.empty())
        ++fp[predicted];
      std::cout << "  miss: expected=" << expected
                << " got=" << (predicted.empty() ? "(none)" : predicted)
                << " | \"" << text << "\"\n";
    }
  }

  std::cout << "eval: " << correct << "/" << total << " top-1 accuracy="
            << (total > 0 ? static_cast<double>(correct) / total : 0.0) << "\n";
  for (const auto& [label, count] : tp) {
    const double p =
        tp[label] + fp[label] > 0
            ? static_cast<double>(tp[label]) / (tp[label] + fp[label])
            : 0.0;
    const double r =
        tp[label] + fn[label] > 0
            ? static_cast<double>(tp[label]) / (tp[label] + fn[label])
            : 0.0;
    std::cout << "  " << label << " precision=" << p << " recall=" << r << "\n";
  }
  return 0;
}

int benchImpl(const std::string& modelPath, const std::string& dataDir,
              int rounds)
{
  const auto t0 = std::chrono::steady_clock::now();
  fasttext::FastText model;
  try {
    model.loadModel(modelPath);
  }
  catch (const std::exception& e) {
    std::cout << "[FAIL] load " << modelPath << ": " << e.what() << "\n";
    return 1;
  }
  const double loadMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
  const auto sizeBytes = std::filesystem::file_size(modelPath);

  const auto samples = readTsv(dataDir + "/check.tsv");
  std::vector<std::string> texts;
  texts.reserve(samples.size());
  for (const auto& c : samples)
    texts.push_back(c.text);
  if (texts.empty())
    texts.push_back("¿qué estás viendo?");

  std::vector<double> latencies;
  latencies.reserve(static_cast<size_t>(rounds));
  for (int i = 0; i < rounds; ++i) {
    const auto t1 = std::chrono::steady_clock::now();
    const auto scores = predictScores(model, texts[i % texts.size()]);
    (void)scores;
    latencies.push_back(std::chrono::duration<double, std::micro>(
                            std::chrono::steady_clock::now() - t1)
                            .count());
  }
  std::sort(latencies.begin(), latencies.end());
  const double p50 = latencies[latencies.size() / 2];
  const double p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
  double sum = 0.0;
  for (double l : latencies)
    sum += l;
  std::cout << "load: " << loadMs << " ms  size: " << sizeBytes / 1024.0
            << " KB\n";
  std::cout << "predict (" << rounds << "): p50=" << p50 << " us p95=" << p95
            << " us avg=" << sum / latencies.size() << " us\n";
  return 0;
}

int queryImpl(const std::string& modelPath, const std::string& text)
{
  fasttext::FastText model;
  try {
    model.loadModel(modelPath);
  }
  catch (const std::exception& e) {
    std::cout << "[FAIL] load " << modelPath << ": " << e.what() << "\n";
    return 1;
  }
  const auto scores = predictScores(model, text);
  for (const auto& [label, score] : scores)
    std::cout << label << "=" << score << "\n";
  return 0;
}

void usage()
{
  std::cout << "argus-intent-probe\n"
            << "  --intent-train [--include-usage] [--epoch N] [--lr L]\n"
            << "                  [--loss softmax|ova|hs|ns]\n"
            << "  --intent-quantize <in.bin> <out.ftz>\n"
            << "  --intent-check [--model path]\n"
            << "  --intent-test <cases.tsv> [--model path]\n"
            << "  --intent-eval [--model path] [valid.tsv]\n"
            << "  --intent-bench [--model path] [rounds]\n"
            << "  --intent-query <text>\n"
            << "  --data <dir>   (default labs/intent-data)\n"
            << "  --model <path> (default models/intent/argus-intent.bin)\n";
}

} // namespace

int main(int argc, char** argv)
{
  std::string dataDir = "labs/intent-data";
  std::string modelPath = "models/intent/argus-intent.bin";
  std::string mode;
  int epoch = 25;
  double lr = 0.5;
  bool includeUsage = false;
  fasttext::loss_name loss = fasttext::loss_name::ova;
  int dim = 50;
  int bucket = 20000;
  int minn = 3;
  int maxn = 6;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data" && i + 1 < argc)
      dataDir = argv[++i];
    else if (arg == "--model" && i + 1 < argc)
      modelPath = argv[++i];
    else if (arg == "--epoch" && i + 1 < argc)
      epoch = std::atoi(argv[++i]);
    else if (arg == "--lr" && i + 1 < argc)
      lr = std::atof(argv[++i]);
    else if (arg == "--loss" && i + 1 < argc) {
      const std::string name = argv[++i];
      if (name == "softmax")
        loss = fasttext::loss_name::softmax;
      else if (name == "hs")
        loss = fasttext::loss_name::hs;
      else if (name == "ns")
        loss = fasttext::loss_name::ns;
      else
        loss = fasttext::loss_name::ova;
    }
    else if (arg == "--dim" && i + 1 < argc)
      dim = std::atoi(argv[++i]);
    else if (arg == "--bucket" && i + 1 < argc)
      bucket = std::atoi(argv[++i]);
    else if (arg == "--minn" && i + 1 < argc)
      minn = std::atoi(argv[++i]);
    else if (arg == "--maxn" && i + 1 < argc)
      maxn = std::atoi(argv[++i]);
    else if (arg == "--include-usage")
      includeUsage = true;
    else if (!arg.empty() && arg[0] == '-' && mode.empty())
      mode = arg;
  }

  if (mode == "--intent-train") {
    const TrainOptions opts = {
        .dataDir = dataDir,
        .outputBase = modelPath.size() > 4
                          ? modelPath.substr(0, modelPath.size() - 4)
                          : modelPath,
        .includeUsage = includeUsage,
        .epoch = epoch,
        .lr = lr,
        .loss = loss,
        .dim = dim,
        .bucket = bucket,
        .minn = minn,
        .maxn = maxn,
    };
    return trainImpl(opts);
  }
  if (mode == "--intent-quantize" && argc >= 4)
    return quantizeImpl(argv[2], argv[3]);
  if (mode == "--intent-check")
    return checkImpl(modelPath, dataDir);
  if (mode == "--intent-test" && argc >= 3)
    return checkFileImpl(modelPath, argv[2]);
  if (mode == "--intent-eval") {
    std::string validPath = dataDir + "/valid.tsv";
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--valid" && i + 1 < argc)
        validPath = argv[++i];
    }
    return evalImpl(modelPath, validPath);
  }
  if (mode == "--intent-bench") {
    int rounds = 1000;
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--rounds" && i + 1 < argc)
        rounds = std::atoi(argv[++i]);
    }
    return benchImpl(modelPath, dataDir, rounds);
  }
  if (mode == "--intent-query" && argc >= 3)
    return queryImpl(modelPath, argv[2]);

  usage();
  return 1;
}
