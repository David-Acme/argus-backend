#include "onnx-session.hxx"

#include <runtime/thread-budget.hxx>

#include <trantor/utils/Logger.h>

#include <onnxruntime_cxx_api.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

namespace turn
{

OnnxSession::OnnxSession() = default;

OnnxSession::~OnnxSession() = default;

std::int64_t residentBytes()
{
  std::ifstream status("/proc/self/statm");
  if (!status.is_open())
    return 0;
  std::int64_t total = 0;
  std::int64_t resident = 0;
  status >> total >> resident;
  if (!status)
    return 0;
  return resident * static_cast<std::int64_t>(::sysconf(_SC_PAGESIZE));
}

bool OnnxSession::open(const std::filesystem::path& modelPath, const OnnxOptions& options)
{
  if (session_)
    return true;
  error_.clear();
  const std::int64_t before = residentBytes();
  try {
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "Argus-llm");
    Ort::SessionOptions sessionOptions;
    const int threads = options.threads > 0 ? options.threads : ThreadBudget::lightThreads();
    sessionOptions.SetIntraOpNumThreads(threads);
    sessionOptions.SetInterOpNumThreads(1);
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (!options.cpuArena)
      sessionOptions.DisableCpuMemArena();
    if (!options.memPattern)
      sessionOptions.DisableMemPattern();
    sessionOptions.AddConfigEntry("session.use_prepacking", options.prepacking ? "1" : "0");
    if (options.mmap) {
      session_ = std::make_unique<Ort::Session>(*env_, modelPath.c_str(), sessionOptions);
    }
    else {
      std::ifstream in(modelPath, std::ios::binary);
      if (!in.is_open()) {
        error_ = "cannot open " + modelPath.string();
        env_.reset();
        return false;
      }
      const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
      session_ = std::make_unique<Ort::Session>(*env_, reinterpret_cast<const void*>(bytes.data()), bytes.size(), sessionOptions);
    }
    const std::int64_t after = residentBytes();
    warmRssDelta_ = after - before;
    const double deltaMb = static_cast<double>(warmRssDelta_) / (1024.0 * 1024.0);
    LOG_INFO << "OnnxSession: opened " << modelPath.filename().string() << " threads=" << threads
             << " cpu_arena=" << options.cpuArena << " mem_pattern=" << options.memPattern
             << " prepacking=" << options.prepacking
             << " mmap=" << options.mmap << " warm_rss_delta_mb=" << deltaMb;
    return true;
  }
  catch (const std::exception& error) {
    error_ = error.what();
    session_.reset();
    env_.reset();
    return false;
  }
}

std::optional<std::vector<OnnxTensor>> OnnxSession::run(const OnnxCall& call)
{
  if (!session_)
    return std::nullopt;
  try {
    const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const Ort::AllocatorWithDefaultOptions allocator;
    std::vector<const char*> inputNames;
    std::vector<Ort::Value> inputValues;
    std::vector<std::vector<std::uint8_t>> flags;
    inputNames.reserve(call.inputs.size());
    inputValues.reserve(call.inputs.size());
    flags.reserve(call.inputs.size());
    for (const OnnxInput& input : call.inputs) {
      inputNames.push_back(input.name.c_str());
      if (input.boolean) {
        std::vector<std::uint8_t>& buffer = flags.emplace_back(input.values.size());
        std::ranges::transform(input.values, buffer.begin(), [](std::int64_t value) { return static_cast<std::uint8_t>(value != 0); });
        inputValues.push_back(Ort::Value::CreateTensor(memory,
                                                       buffer.data(),
                                                       buffer.size(),
                                                       input.shape.data(),
                                                       input.shape.size(),
                                                       ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
        continue;
      }
      inputValues.push_back(Ort::Value::CreateTensor(memory,
                                                     const_cast<std::int64_t*>(input.values.data()),
                                                     input.values.size() * sizeof(std::int64_t),
                                                     input.shape.data(),
                                                     input.shape.size(),
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64));
    }
    std::vector<const char*> outputNames;
    outputNames.reserve(call.outputs.size());
    for (const std::string& name : call.outputs)
      outputNames.push_back(name.c_str());
    std::vector<Ort::Value> outputs = session_->Run(Ort::RunOptions{nullptr},
                                                    inputNames.data(),
                                                    inputValues.data(),
                                                    inputValues.size(),
                                                    outputNames.data(),
                                                    outputNames.size());
    std::vector<OnnxTensor> out;
    out.reserve(outputs.size());
    for (Ort::Value& value : outputs) {
      const auto info = value.GetTensorTypeAndShapeInfo();
      const std::vector<std::int64_t> shape = info.GetShape();
      OnnxTensor tensor{.shape = shape, .values = {}, .whole = {}, .integral = false};
      std::size_t size = 1;
      for (const std::int64_t dim : shape)
        size *= static_cast<std::size_t>(dim > 0 ? dim : 0);
      if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
        tensor.integral = true;
        const std::int64_t* data = value.GetTensorData<std::int64_t>();
        tensor.whole.assign(data, data + size);
      }
      else {
        const float* data = value.GetTensorData<float>();
        tensor.values.assign(data, data + size);
      }
      out.push_back(std::move(tensor));
    }
    return out;
  }
  catch (const std::exception& error) {
    error_ = error.what();
    LOG_WARN << "OnnxSession: run failed: " << error_;
    return std::nullopt;
  }
}

}
