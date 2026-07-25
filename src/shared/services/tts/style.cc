#include "style.hxx"

Style::Style(std::vector<float> ttlData, std::vector<int64_t> ttlShape,
             std::vector<float> dpData, std::vector<int64_t> dpShape)
    : ttlData_(std::move(ttlData)), dpData_(std::move(dpData)),
      ttlShape_(std::move(ttlShape)), dpShape_(std::move(dpShape))
{
}
