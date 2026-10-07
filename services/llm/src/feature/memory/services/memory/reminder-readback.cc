#include "reminder-readback.hxx"

#include <text/spoken-time.hxx>

#include <string_view>

namespace reminder_readback
{

namespace
{
std::string spokenAt(const Spoken& spoken, bool bare)
{
  return spoken_time::moment(
      {.epoch = spoken.fireAt, .now = spoken.now, .lang = spoken.lang, .day = spoken_time::Day::Weekday, .bare = bare});
}

std::string_view reasonOf(const Spoken& spoken)
{
  const bool english = spoken.lang == "en";
  switch (spoken.why) {
    case ReminderCallOutcome::TooFar:
      return english ? " I can only call you up to 12 months ahead." : " Solo puedo llamarte hasta dentro de 12 meses.";
    case ReminderCallOutcome::InThePast:
      return english ? " That time has already passed." : " Esa hora ya pasó.";
    case ReminderCallOutcome::TooMany:
      return english ? " You already have a lot of calls waiting." : " Ya tienes muchas llamadas pendientes.";
    case ReminderCallOutcome::Unavailable:
      return english ? " The call service is not answering right now." : " El servicio de llamadas no responde ahora.";
    case ReminderCallOutcome::Scheduled:
    case ReminderCallOutcome::Refused:
      break;
  }
  return {};
}
}

std::string moment(const Spoken& spoken)
{
  return spokenAt(spoken, false);
}

std::string sentence(const Spoken& spoken)
{
  const bool english = spoken.lang == "en";
  switch (spoken.call) {
    case Call::Scheduled:
      return (english ? " I will call you " : " Te llamaré ") + spokenAt(spoken, false) + ".";
    case Call::NotAttempted:
      return (english ? " It is in your reminders for " : " Quedó en tus recordatorios para ") + spokenAt(spoken, true) + ".";
    case Call::Failed:
      break;
  }
  if (english)
    return std::string(spoken.listed ? " I saved it in your reminders, but I could not schedule the call."
                                     : " I saved it, but I could not schedule the call.") +
           std::string(reasonOf(spoken)) + " It was for " + spokenAt(spoken, true) + ".";
  return std::string(spoken.listed ? " Lo guardé en tus recordatorios, pero no pude programar la llamada."
                                   : " Lo guardé, pero no pude programar la llamada.") +
         std::string(reasonOf(spoken)) + " Era para " + spokenAt(spoken, false) + ".";
}

}
