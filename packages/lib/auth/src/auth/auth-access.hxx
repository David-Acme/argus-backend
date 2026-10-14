#pragma once

#include <memory>

class AuthClient;

std::shared_ptr<const AuthClient> filterAuthClient();

void requireAuthTarget();

void installLocalAuthClient(std::shared_ptr<const AuthClient> client);
