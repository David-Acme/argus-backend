#pragma once

#include <memory>

class IdentityClient;

// Shared identity RPC client of the filter chain, cached per resolved target.
std::shared_ptr<const IdentityClient> filterIdentityClient();
