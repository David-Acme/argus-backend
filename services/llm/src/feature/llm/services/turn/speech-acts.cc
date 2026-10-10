#include "speech-acts.hxx"

#include <type_traits>

namespace turn::speech
{

std::string_view actName(const Act& act)
{
  return std::visit(
      [](const auto& value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AskSlot>)
          return "ask_slot";
        else if constexpr (std::is_same_v<T, Confirm>)
          return "confirm";
        else if constexpr (std::is_same_v<T, Choose>)
          return "choose";
        else if constexpr (std::is_same_v<T, Done>)
          return "done";
        else if constexpr (std::is_same_v<T, Refused>)
          return "refused";
        else if constexpr (std::is_same_v<T, Offer>)
          return "offer";
        else if constexpr (std::is_same_v<T, Declined>)
          return "declined";
        else if constexpr (std::is_same_v<T, Unactionable>)
          return "unactionable";
        else if constexpr (std::is_same_v<T, Capability>)
          return "capability";
        else
          return "misunderstood";
      },
      act);
}

bool isQuestion(const Act& act)
{
  return std::holds_alternative<AskSlot>(act) || std::holds_alternative<Confirm>(act) ||
         std::holds_alternative<Choose>(act);
}

std::string_view slotOf(const Act& act)
{
  const auto* ask = std::get_if<AskSlot>(&act);
  return ask == nullptr ? std::string_view{} : std::string_view(ask->slot);
}

std::string_view reasonName(AskReason reason)
{
  switch (reason) {
    case AskReason::Missing:
      return "missing";
    case AskReason::AmbiguousDate:
      return "ambiguous_date";
    case AskReason::DatePassed:
      return "date_passed";
    case AskReason::BeyondRange:
      return "beyond_range";
    case AskReason::ProjectChoice:
      return "project_choice";
    case AskReason::ProjectName:
      return "project_name";
    case AskReason::ProjectNoneYet:
      return "project_none_yet";
  }
  return "missing";
}

}
