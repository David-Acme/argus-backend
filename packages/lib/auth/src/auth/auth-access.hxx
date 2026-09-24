#pragma once

#include <memory>

class AuthClient;

std::shared_ptr<const AuthClient> filterAuthClient();
