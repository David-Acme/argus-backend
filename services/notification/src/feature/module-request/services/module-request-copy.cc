#include "module-request-copy.hxx"

ModuleRequestText module_request_copy::render(const ModuleRequestCopyInput& input)
{
  const std::string who = input.requester.empty() ? std::string(input.lang == "en" ? "Someone" : "Alguien")
                                                  : std::string(input.requester);
  if (input.lang == "en")
    return {.title = "Module request",
            .body = who + " would like to use the " + std::string(input.module) + " module. Turn it on?"};
  return {.title = "Piden un módulo",
          .body = who + " quiere usar el módulo " + std::string(input.module) + ". ¿Lo activas?"};
}
