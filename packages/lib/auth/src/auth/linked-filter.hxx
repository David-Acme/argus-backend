#pragma once

#include <drogon/DrClassMap.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace auth_filters
{

template <typename Filter>
void requireLinked()
{
  const auto names = drogon::DrClassMap::getAllClassName();
  if (std::ranges::find(names, Filter::classTypeName()) == names.end())
    throw std::runtime_error(std::string(Filter::classTypeName()) + " is not linked into this binary");
}

}
