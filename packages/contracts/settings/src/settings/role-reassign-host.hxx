#pragma once

#include <settings/component-vocabulary.hxx>

class RoleReassignHost
{
public:
  RoleReassignHost() = default;
  virtual ~RoleReassignHost() = default;
  RoleReassignHost(const RoleReassignHost&) = delete;
  RoleReassignHost& operator=(const RoleReassignHost&) = delete;
  RoleReassignHost(RoleReassignHost&&) = delete;
  RoleReassignHost& operator=(RoleReassignHost&&) = delete;

  virtual RoleReassignmentOutcome reassign(const RoleReassignmentBatch& batch) = 0;
};
