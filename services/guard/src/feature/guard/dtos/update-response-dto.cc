#include "update-response-dto.hxx"

#include <algorithm>
#include <regex>
#include <validation/validation_dsl.hxx>

namespace
{
constexpr std::size_t kMaxRecipients = 64;
constexpr std::size_t kMaxContacts = 10;
constexpr std::size_t kMaxName = 60;
constexpr std::size_t kMaxNote = 60;
constexpr int kMaxStep = 32;
constexpr int kMinStepSeconds = 15;
constexpr int kMaxStepSeconds = 300;

std::string trimmed(const std::string& text)
{
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

bool validPhone(const std::string& phone)
{
  static const std::regex pattern(R"(^\+?[0-9][0-9 ()\-]{1,22}$)");
  const auto digits =
      std::ranges::count_if(phone, [](char c) { return c >= '0' && c <= '9'; });
  return digits >= 3 && std::regex_match(phone, pattern);
}

bool validEmergency(const std::string& number)
{
  static const std::regex pattern(R"(^[0-9+*#]{2,16}$)");
  return number.empty() || std::regex_match(number, pattern);
}

std::string parseRecipients(const Json::Value& array,
                            std::vector<ResponseRecipientDto>& out)
{
  if (!array.isArray())
    return "must be a list";
  if (array.size() > kMaxRecipients)
    return "must have at most 64 people";
  for (Json::ArrayIndex index = 0; index < array.size(); ++index) {
    const Json::Value& item = array[index];
    const std::string where = "item " + std::to_string(index + 1) + " ";
    if (!item.isObject() || !item["userId"].isIntegral() ||
        !item["mode"].isString() || !item["step"].isInt() ||
        (item.isMember("onDuty") && !item["onDuty"].isBool()))
      return where + "needs userId, mode, step and an optional onDuty";
    ResponseRecipientDto recipient{.userId = item["userId"].asInt64(),
                                   .mode = item["mode"].asString(),
                                   .step = item["step"].asInt(),
                                   .onDuty =
                                       item.get("onDuty", false).asBool()};
    if (recipient.userId <= 0)
      return where + "has an invalid userId";
    if (recipient.mode != "call" && recipient.mode != "notify" &&
        recipient.mode != "off")
      return where + "mode must be call, notify or off";
    if (recipient.step < 0 || recipient.step > kMaxStep)
      return where + "step must be between 0 and 32";
    if (std::ranges::find(out, recipient.userId,
                          &ResponseRecipientDto::userId) != out.end())
      return where + "repeats a person";
    out.push_back(std::move(recipient));
  }
  return {};
}

std::string parseContacts(const Json::Value& array,
                          std::vector<ResponseContactDto>& out)
{
  if (!array.isArray())
    return "must be a list";
  if (array.size() > kMaxContacts)
    return "must have at most 10 contacts";
  for (Json::ArrayIndex index = 0; index < array.size(); ++index) {
    const Json::Value& item = array[index];
    const std::string where = "item " + std::to_string(index + 1) + " ";
    if (!item.isObject() || !item["name"].isString() ||
        !item["phone"].isString() ||
        (item.isMember("note") && !item["note"].isString()))
      return where + "needs name, phone and an optional note";
    ResponseContactDto contact{.name = trimmed(item["name"].asString()),
                               .phone = trimmed(item["phone"].asString()),
                               .note =
                                   trimmed(item.get("note", "").asString())};
    if (contact.name.empty() || contact.name.size() > kMaxName)
      return where + "name must be 1-60 characters";
    if (!validPhone(contact.phone))
      return where + "phone must be a phone number";
    if (contact.note.size() > kMaxNote)
      return where + "note must be at most 60 characters";
    out.push_back(std::move(contact));
  }
  return {};
}
}

UpdateResponseDto UpdateResponseDto::fromJson(const Json::Value& json)
{
  UpdateResponseDto dto;
  if (json.isMember("emergencyNumber")) {
    if (json["emergencyNumber"].isString())
      dto.emergencyNumber = trimmed(json["emergencyNumber"].asString());
    else
      dto.typeError = "emergencyNumber";
  }
  if (json.isMember("stepSeconds")) {
    if (json["stepSeconds"].isInt())
      dto.stepSeconds = json["stepSeconds"].asInt();
    else
      dto.typeError += dto.typeError.empty() ? "stepSeconds" : ", stepSeconds";
  }
  dto.recipientsError = parseRecipients(json["recipients"], dto.recipients);
  dto.contactsError = json.isMember("contacts")
                          ? parseContacts(json["contacts"], dto.contacts)
                          : std::string{};

  START_VALIDATION(UpdateResponseDto, dto)
  CUSTOM_LAMBDA(body,
                [](const UpdateResponseDto& value)
                    -> std::optional<std::string> {
                  if (value.typeError.empty())
                    return std::nullopt;
                  return "wrong type for " + value.typeError;
                })
  CUSTOM_LAMBDA(recipients,
                [](const UpdateResponseDto& value)
                    -> std::optional<std::string> {
                  if (value.recipientsError.empty())
                    return std::nullopt;
                  return value.recipientsError;
                })
  CUSTOM_LAMBDA(contacts,
                [](const UpdateResponseDto& value)
                    -> std::optional<std::string> {
                  if (value.contactsError.empty())
                    return std::nullopt;
                  return value.contactsError;
                })
  CUSTOM_LAMBDA(emergencyNumber,
                [](const UpdateResponseDto& value)
                    -> std::optional<std::string> {
                  if (validEmergency(value.emergencyNumber))
                    return std::nullopt;
                  return "must be 2-16 digits (+, * and # allowed)";
                })
  CUSTOM_LAMBDA(stepSeconds,
                [](const UpdateResponseDto& value)
                    -> std::optional<std::string> {
                  if (value.stepSeconds >= kMinStepSeconds &&
                      value.stepSeconds <= kMaxStepSeconds)
                    return std::nullopt;
                  return "must be between 15 and 300";
                })
  END_VALIDATION()
  return dto;
}
