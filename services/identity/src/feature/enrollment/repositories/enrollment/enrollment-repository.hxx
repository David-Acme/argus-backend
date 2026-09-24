#pragma once

#include "enrollment-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>

class EnrollmentRepository
{
public:
  EnrollmentRepository() = default;
  ~EnrollmentRepository() = default;

  [[nodiscard]] drogon::Task<int64_t>
  countUsers(drogon::orm::DbClient* client) const;

  [[nodiscard]] drogon::Task<int64_t>
  insertUser(const EnrollmentUserInput& input) const;

  [[nodiscard]] drogon::Task<int64_t>
  insertPerson(const EnrollmentPersonInput& input) const;

  [[nodiscard]] drogon::Task<int64_t>
  insertFaceEmbedding(const EnrollmentFaceInput& input) const;

  [[nodiscard]] drogon::Task<bool>
  consumeInvitation(const EnrollmentInvitationConsumeInput& input) const;

  [[nodiscard]] drogon::Task<void>
  insertRedemption(const EnrollmentRedemptionInput& input) const;
};
