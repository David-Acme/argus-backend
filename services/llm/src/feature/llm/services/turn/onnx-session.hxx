#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Ort
{
struct Env;
struct Session;
}

namespace turn
{

enum class EngineStatus : unsigned char
{
  Ready,
  Unavailable
};

struct OnnxOptions
{
  int threads{0};
  bool cpuArena{false};
  bool memPattern{false};
  bool prepacking{false};
  bool mmap{true};
};

struct OnnxInput
{
  std::string name;
  std::vector<int64_t> shape;
  std::vector<int64_t> values;
  bool boolean{false};
};

struct OnnxTensor
{
  std::vector<int64_t> shape;
  std::vector<float> values;
};

struct OnnxCall
{
  std::vector<OnnxInput> inputs;
  std::vector<std::string> outputs;
};

class OnnxSession
{
public:
  OnnxSession();
  ~OnnxSession();

  OnnxSession(const OnnxSession&) = delete;
  OnnxSession& operator=(const OnnxSession&) = delete;

  [[nodiscard]] bool open(const std::filesystem::path& modelPath, const OnnxOptions& options);

  [[nodiscard]] bool loaded() const { return session_ != nullptr; }

  [[nodiscard]] const std::string& error() const { return error_; }

  [[nodiscard]] std::int64_t warmRssDeltaBytes() const { return warmRssDelta_; }

  [[nodiscard]] std::optional<std::vector<OnnxTensor>> run(const OnnxCall& call);

private:
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::string error_;
  std::int64_t warmRssDelta_{0};
};

[[nodiscard]] std::int64_t residentBytes();

}
