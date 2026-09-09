#pragma once

#include <json/value.h>

// Subject handler for the productivity and notification change funnels
// (Rulings AQ/AR): a `kind: audit` event carries the exact user_audit_log
// diff the producer would have written, so the gateway inserts the rows
// verbatim into its user_audit_log substrate BEFORE fanning the DB-assigned
// row out to the user's room (Ruling Y); anything else is a plain user-scoped
// change event that fans out as-is.
namespace user_change_fan_out
{
void handleUserChange(const Json::Value& json);
} // namespace user_change_fan_out
