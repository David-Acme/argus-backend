#pragma once

struct HealthThresholds
{
  double dark{25.0};
  double bright{235.0};
  double blur{18.0};
  double sceneDiff{0.35};
  double saturatedShare{0.6};
  double whiteoutEdgeRatio{0.15};
  double blindEdgeRatio{0.5};
  double coverEntropy{3.0};
  double coverEdgeRatio{0.2};
  double partialBlockShare{0.28};
  double defocusSharpness{0.4};
  double reaimShiftPx{5.0};
  double infraredChroma{4.0};
};
