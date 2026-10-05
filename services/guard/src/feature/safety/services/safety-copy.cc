#include "safety-copy.hxx"

SafetyText safety_copy::panicSent(std::string_view lang)
{
  if (lang == "en")
    return {.title = "Alert sent",
            .body = "Your household was alerted silently. If you can, get somewhere safe."};
  return {.title = "Aviso enviado",
          .body = "Hemos avisado en silencio a tu casa. Si puedes, ponte a salvo."};
}
