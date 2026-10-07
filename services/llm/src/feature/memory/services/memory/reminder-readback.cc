#include "reminder-readback.hxx"

#include <text/spoken-time.hxx>

namespace reminder_readback
{

std::string sentence(const Spoken& spoken)
{
  const spoken_time::When when{.epoch = spoken.fireAt,
                               .now = spoken.now,
                               .lang = spoken.lang,
                               .day = spoken_time::Day::Weekday,
                               .bare = !spoken.called};
  const std::string moment = spoken_time::moment(when);
  if (spoken.lang == "en")
    return (spoken.called ? " I will call you " : " It is in your reminders for ") + moment + ".";
  return (spoken.called ? " Te llamaré " : " Quedó en tus recordatorios para ") + moment + ".";
}

}
