#pragma once

#include <memory>

class IdentityClient;

// Shared identity RPC client of the filter chain, cached per resolved target
// (identity.target, else identity.rpc_host / identity.rpc_port defaulting to
// 127.0.0.1:7040).
std::shared_ptr<const IdentityClient> filterIdentityClient();
