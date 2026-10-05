#include "response-copy.hxx"

#include <feature/call/services/call-copy.hxx>

namespace
{
std::string contactList(const Json::Value& contacts, bool english)
{
  std::string list;
  const Json::ArrayIndex count = contacts.isArray() ? contacts.size() : 0;
  const Json::ArrayIndex shown = count < 2 ? count : 2;
  for (Json::ArrayIndex index = 0; index < shown; ++index) {
    const Json::Value& contact = contacts[index];
    if (index > 0)
      list += english ? " or " : " o ";
    list += contact.get("name", "").asString() + " (" +
            contact.get("phone", "").asString() + ")";
  }
  return list;
}
}

std::string response_copy::escalationBody(const ResponseEscalationInput& input)
{
  const bool english = call_copy::normalizeLang(input.lang) == "en";
  switch (input.reason) {
    case ResponseReach::Worse:
      return input.summary + (english ? " It has got worse." : " La situación ha empeorado.");
    case ResponseReach::Confirmed:
      if (input.confirmedBy.empty())
        return input.summary + (english ? " It is confirmed as real." : " Está confirmado que es real.");
      return input.summary + (english ? " " + input.confirmedBy + " confirmed it is real."
                                      : " " + input.confirmedBy + " ha confirmado que es real.");
    case ResponseReach::NextStep:
      break;
  }
  return input.summary +
         (english ? " Nobody has answered yet." : " Nadie ha contestado todavía.");
}

ResponseNotice response_copy::contacts(const ResponseContactsInput& input)
{
  const bool english = call_copy::normalizeLang(input.lang) == "en";
  const std::string suffix = input.place.empty() ? std::string{} : " · " + input.place;
  ResponseNotice notice;
  if (input.confirmed) {
    const std::string who = input.confirmedBy.empty()
                                ? std::string(english ? "Someone" : "Alguien")
                                : input.confirmedBy;
    notice.title = (english ? "Confirmed alert" : "Alerta confirmada") + suffix;
    notice.body = english ? who + " confirmed it is real."
                          : who + " ha confirmado que es real.";
  }
  else {
    notice.title = (english ? "Nobody answered" : "Nadie ha contestado") + suffix;
    notice.body = english ? "Nobody in the household answered the call."
                          : "Nadie de casa ha contestado la llamada.";
  }
  const std::string list = contactList(input.contacts, english);
  if (!list.empty() && !input.emergencyNumber.empty())
    notice.body += english ? " Call " + list + ", or " + input.emergencyNumber +
                                 " if it is an emergency."
                           : " Llama a " + list + ", o al " + input.emergencyNumber +
                                 " si es una emergencia.";
  else if (!list.empty())
    notice.body += (english ? " Call " : " Llama a ") + list + ".";
  else if (!input.emergencyNumber.empty())
    notice.body += english ? " Call " + input.emergencyNumber + " if it is an emergency."
                           : " Llama al " + input.emergencyNumber +
                                 " si es una emergencia.";
  return notice;
}
