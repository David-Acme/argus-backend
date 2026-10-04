#include "setting-names.hxx"

std::string settingTypeName(SettingType type)
{
  switch (type) {
  case SettingType::Toggle: return "toggle";
  case SettingType::Integer: return "integer";
  case SettingType::Decimal: return "decimal";
  case SettingType::Choice: return "choice";
  case SettingType::Text: return "text";
  }
  return "text";
}

std::string settingLevelName(SettingLevel level)
{
  return level == SettingLevel::Basic ? "basic" : "advanced";
}

std::string settingApplyName(SettingApply apply)
{
  switch (apply) {
  case SettingApply::Live: return "live";
  case SettingApply::NextSession: return "nextSession";
  case SettingApply::Restart: return "restart";
  }
  return "restart";
}

std::string choiceAvailabilityName(ChoiceAvailability availability)
{
  switch (availability) {
  case ChoiceAvailability::Installed: return "installed";
  case ChoiceAvailability::Installable: return "installable";
  case ChoiceAvailability::Installing: return "installing";
  case ChoiceAvailability::HostOnly: return "hostOnly";
  case ChoiceAvailability::Failed: return "failed";
  }
  return "installed";
}

std::string rejectionReasonName(SettingRejectionReason reason)
{
  switch (reason) {
  case SettingRejectionReason::Unknown: return "unknownKey";
  case SettingRejectionReason::Invalid: return "invalid";
  case SettingRejectionReason::OutOfRange: return "outOfRange";
  case SettingRejectionReason::NotAChoice: return "notAChoice";
  case SettingRejectionReason::WriteFailed: return "writeFailed";
  case SettingRejectionReason::NotInstalled: return "notInstalled";
  }
  return "invalid";
}
