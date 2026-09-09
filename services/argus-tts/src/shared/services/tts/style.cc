#include "style.hxx"

Style::Style(StyleDeps deps)
    : ttlData_(std::move(deps.ttlData)), dpData_(std::move(deps.dpData)),
      ttlShape_(std::move(deps.ttlShape)), dpShape_(std::move(deps.dpShape))
{
}
