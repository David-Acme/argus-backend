#pragma once

#include <atomic>
#include <cstdint>
#include <feature/voiceprint/repositories/voiceprint/voiceprint-repository.hxx>
#include <mutex>
#include <span>
#include <sqlite/vec-db.hxx>
#include <string>
#include <vector>

struct VoiceprintIndexInit
{
  int dims{0};
  std::string model;
};

struct VoiceprintIndexInsert
{
  int64_t voiceprintId{0};
  int64_t userId{0};
  std::span<const float> embedding;
};

struct VoiceprintNearest
{
  int64_t voiceprintId{0};
  int64_t userId{0};
  float similarity{0.0F};
};

class VoiceprintIndex
{
public:
  explicit VoiceprintIndex(VecDb& vecDb) : vecDb_(vecDb) {}

  static VoiceprintIndex& instance();

  bool init(const VoiceprintIndexInit& input);
  bool insert(const VoiceprintIndexInsert& input);
  void remove(int64_t voiceprintId);

  [[nodiscard]] std::vector<VoiceprintNearest>
  nearest(std::span<const float> query, int count);

  [[nodiscard]] size_t size() const;

private:
  VecDb& vecDb_;
  VoiceprintRepository repository_;
  int dims_{0};
  std::atomic<size_t> size_{0};
};
