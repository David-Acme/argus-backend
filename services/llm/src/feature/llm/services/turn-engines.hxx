#pragma once

#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/deciders.hxx>
#include <feature/llm/services/turn/gliner-extractor.hxx>
#include <feature/llm/services/turn/laya-decider.hxx>
#include <feature/llm/services/turn/onnx-session.hxx>

#include <filesystem>
#include <memory>
#include <string>

class LfmAdapter;

struct TurnBundle
{
  std::filesystem::path dir;
  std::string pin;
  turn::OnnxOptions options;
};

struct TurnEngineInput
{
  LfmAdapter& adapter;
  TurnBundle decide;
  TurnBundle extract;
};

struct TurnEngines
{
  std::unique_ptr<turn::FirstOf> stack;
  std::unique_ptr<turn::LayaDecider> laya;
  std::unique_ptr<turn::GlinerExtractor> gliner;
  std::string decideError;
  std::string extractError;

  [[nodiscard]] std::string pipeline() const;
};

[[nodiscard]] TurnEngines installTurnEngines(const TurnEngineInput& input);
