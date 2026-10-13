#include <feature/llm/services/turn/bundle-golden.hxx>
#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/gliner-extractor.hxx>
#include <feature/llm/services/turn/laya-decider.hxx>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr int kPassed = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;
constexpr int kNoGolden = 3;
constexpr int kSkipped = 77;

int usage()
{
  std::cerr << "usage: llm-bundle-check <decide|extract> <bundle-dir> [--skip-missing]\n"
               "Validates a bundle as argus-llm loads it, then runs its model on its own golden vectors.\n"
               "Exit 0 passed, 1 failed, 2 usage or invalid bundle, 3 no golden vectors; with --skip-missing an\n"
               "absent bundle or one without golden vectors exits 77.\n";
  return kUsage;
}
}

int main(int argc, char** argv)
{
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.size() < 2 || (args[0] != "decide" && args[0] != "extract"))
    return usage();
  const bool decide = args[0] == "decide";
  const std::filesystem::path dir(args[1]);
  const bool skipMissing = args.size() > 2 && args[2] == "--skip-missing";
  if (skipMissing && !std::filesystem::exists(dir / "model.onnx")) {
    std::cout << "SKIPPED: no bundle at " << dir.string() << "\n";
    return kSkipped;
  }
  const turn::BundleLoader bundle({.dir = dir, .kind = decide ? turn::BundleKind::Decider : turn::BundleKind::Extractor});
  if (!bundle.valid()) {
    std::cerr << "llm-bundle-check: " << bundle.error() << "\n";
    return kUsage;
  }
  std::cout << "bundle " << dir.filename().string() << ": model.onnx " << bundle.modelSha256().substr(0, 12) << ", precision "
            << turn::bundlePrecisionToString(bundle.precision()) << "\n";
  if (!bundle.golden().present) {
    std::cout << (skipMissing ? "SKIPPED: " : "") << "the bundle carries no golden vectors; its model was not checked\n";
    return skipMissing ? kSkipped : kNoGolden;
  }
  turn::GoldenReport report;
  if (decide) {
    const std::unique_ptr<turn::LayaModel> model = turn::openLayaModel({.bundle = bundle, .options = {}, .decode = bundle.decode()});
    report = turn::checkLayaGolden({.bundle = bundle, .model = *model});
  }
  else {
    const std::unique_ptr<turn::GlinerModel> model = turn::openGlinerModel(bundle, {});
    report = turn::checkGlinerGolden({.bundle = bundle, .model = *model});
  }
  std::cout << "golden: " << turn::describeGolden(report) << "\n";
  return report.passed ? kPassed : kFailed;
}
