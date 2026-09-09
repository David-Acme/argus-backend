#pragma once

#include <cstdint>
#include <vector>

class Style
{
public:
  Style(std::vector<float> ttlData, std::vector<int64_t> ttlShape,
        std::vector<float> dpData, std::vector<int64_t> dpShape);
  ~Style() = default;

  const std::vector<float>& ttlData() const { return ttlData_; }
  const std::vector<float>& dpData() const { return dpData_; }
  const std::vector<int64_t>& ttlShape() const { return ttlShape_; }
  const std::vector<int64_t>& dpShape() const { return dpShape_; }

private:
  std::vector<float> ttlData_;
  std::vector<float> dpData_;
  std::vector<int64_t> ttlShape_;
  std::vector<int64_t> dpShape_;
};
