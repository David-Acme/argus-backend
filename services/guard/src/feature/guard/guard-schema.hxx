#pragma once

#include <feature/guard/repositories/environment/environment-query.hxx>

#include <string>

namespace guard_schema
{

bool migrate(const std::string& schemaPath);

bool seedEnvironments(const GuardEnvironment& seed);

}
