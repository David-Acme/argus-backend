#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/reply-claims.hxx>

#include <string>
#include <vector>

namespace
{
bool claims(const std::string& text, bool asked = false)
{
  return reply_claims::claimsDone({.text = text, .asked = asked});
}

struct Spoken
{
  std::string text;
  int doneCalls{0};
};

std::string run(reply_claims::ClaimGate& gate, const std::vector<std::string>& tokens)
{
  const TokenCallback callback = gate.callback();
  for (const auto& token : tokens)
    callback(token, false);
  callback("", true);
  return gate.spoken();
}
}

TEST_CASE("a first-person completion is a claim in Spanish, Peruvian Spanish and English")
{
  CHECK(claims("Agendé la reunión con Andrea para el jueves."));
  CHECK(claims("Ya te lo agendé, no te preocupes."));
  CHECK(claims("Programé el recordatorio para las tres."));
  CHECK(claims("He guardado tu nota."));
  CHECK(claims("Ya lo anoté pues, jefe."));
  CHECK(claims("Te lo dejé anotado."));
  CHECK(claims("Activé el módulo de productividad."));
  CHECK(claims("Cancelé la cita de mañana."));
  CHECK(claims("Creo una reunión con Andrea para el jueves a las 3:00 PM."));
  CHECK(claims("Agendo la reunión ahora mismo."));
  CHECK(claims("Guardo tu contraseña del wifi."));
  CHECK(claims("Quedó agendada para el viernes."));
  CHECK(claims("I've scheduled the meeting with Andrea."));
  CHECK(claims("I scheduled it for Thursday."));
  CHECK(claims("I have created the task."));
  CHECK(claims("I'm adding it to your agenda."));
  CHECK(claims("I turned on the productivity module."));
  CHECK(claims("Sure, I cancelled the appointment."));
}

TEST_CASE("a bare marker is a claim only after the user asked for something")
{
  CHECK(claims("Confirmado.", true));
  CHECK(claims("Listo.", true));
  CHECK(claims("Hecho, ya está.", true));
  CHECK(claims("Done.", true));
  CHECK(claims("All set.", true));
  CHECK(claims("Your meeting is scheduled.", true));
  CHECK(claims("Creo una reunión con Andrea para el jueves a las 3:00 PM. Confirmado.", true));
  CHECK_FALSE(claims("Confirmado.", false));
  CHECK_FALSE(claims("Listo, ¿en qué más te ayudo?", false));
  CHECK_FALSE(claims("Done.", false));
}

TEST_CASE("an offer, a question, a plan, a refusal or an opinion is not a claim")
{
  CHECK_FALSE(claims("¿Quieres que lo agende para el jueves?", true));
  CHECK_FALSE(claims("¿Lo agendé bien?", true));
  CHECK_FALSE(claims("Puedo agendarlo si quieres.", true));
  CHECK_FALSE(claims("Voy a agendar la reunión en cuanto me digas la hora.", true));
  CHECK_FALSE(claims("Cuando lo agende te aviso.", true));
  CHECK_FALSE(claims("No pude agendarlo porque la agenda está apagada.", true));
  CHECK_FALSE(claims("No lo agendé todavía.", true));
  CHECK_FALSE(claims("Todavía no lo he guardado.", true));
  CHECK_FALSE(claims("Creo que mañana llueve.", true));
  CHECK_FALSE(claims("Ana cree que sí.", true));
  CHECK_FALSE(claims("Would you like me to schedule it?", true));
  CHECK_FALSE(claims("I can schedule it for you if you want.", true));
  CHECK_FALSE(claims("I haven't scheduled anything yet.", true));
  CHECK_FALSE(claims("I could not save it.", true));
  CHECK_FALSE(claims("I will schedule it once you tell me the time.", true));
  CHECK_FALSE(claims("Productividad está apagada. ¿Quieres que la active?", true));
  CHECK_FALSE(claims("El módulo está activo.", true));
  CHECK_FALSE(claims("Tienes tres eventos mañana.", false));
}

TEST_CASE("a claim before a question in the same breath is still a claim")
{
  CHECK(claims("Listo, ya lo agendé, ¿algo más?", true));
  CHECK(claims("I've scheduled it, anything else?", true));
  CHECK_FALSE(claims("Did you want me to schedule it, or not?", true));
}

TEST_CASE("a request to act is recognized by its verb in both languages")
{
  CHECK(reply_claims::asksForAction("Agéndame una reunión con Andrea el jueves a las tres"));
  CHECK(reply_claims::asksForAction("agenda una reunión con Pedro"));
  CHECK(reply_claims::asksForAction("Por favor, anótame que compré leche"));
  CHECK(reply_claims::asksForAction("¿Puedes programarme una cita?"));
  CHECK(reply_claims::asksForAction("Activa el módulo de productividad"));
  CHECK(reply_claims::asksForAction("Crea una tarea para llamar al dentista"));
  CHECK(reply_claims::asksForAction("Schedule a meeting with Andrea on Thursday"));
  CHECK(reply_claims::asksForAction("Can you add a task to call the dentist?"));
  CHECK(reply_claims::asksForAction("Please turn on the productivity module"));
  CHECK_FALSE(reply_claims::asksForAction("¿Qué tengo en mi agenda?"));
  CHECK_FALSE(reply_claims::asksForAction("Gracias"));
  CHECK_FALSE(reply_claims::asksForAction("How is the weather?"));
}

TEST_CASE("the honest reply and the nudge come in the user's language")
{
  CHECK(reply_claims::honest("es") == "No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(reply_claims::honest("en") == "I could not do it. Shall I try again?");
  CHECK(reply_claims::honest("fr") == reply_claims::honest("es"));
  CHECK(reply_claims::nudge("es").find("llama ahora a su herramienta") != std::string::npos);
  CHECK(reply_claims::nudge("en").find("call its tool now") != std::string::npos);
}

TEST_CASE("the gate lets clean sentences through whole and in order")
{
  std::string heard;
  int done = 0;
  bool legitimate = false;
  reply_claims::ClaimGate gate({.sink = [&](const std::string& token, bool last) {
                                  heard += token;
                                  if (last)
                                    ++done;
                                },
                                .lang = "es",
                                .asked = true,
                                .legitimate = [&legitimate] { return legitimate; }});
  const std::string spoken = run(gate, {"Tienes ", "tres eventos", " mañana.", " El primero ", "es a las diez."});
  CHECK(spoken == "Tienes tres eventos mañana. El primero es a las diez.");
  CHECK(heard == spoken);
  CHECK(done == 1);
  CHECK_FALSE(gate.cut());
}

TEST_CASE("the gate holds a sentence until it ends and cuts a claim nobody earned")
{
  std::string heard;
  int done = 0;
  reply_claims::ClaimGate gate({.sink = [&](const std::string& token, bool last) {
                                  heard += token;
                                  if (last)
                                    ++done;
                                },
                                .lang = "es",
                                .asked = true,
                                .legitimate = [] { return false; }});
  const TokenCallback callback = gate.callback();
  callback("Claro", false);
  callback(" que sí.", false);
  CHECK(heard == "Claro que sí.");
  callback(" Creo una reunión con ", false);
  CHECK(heard == "Claro que sí.");
  callback("Andrea para el jueves.", false);
  CHECK(heard == "Claro que sí. No pude hacerlo. ¿Lo intento de nuevo?");
  callback(" Confirmado.", false);
  callback("", true);
  CHECK(heard == "Claro que sí. No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(done == 1);
  CHECK(gate.cut());
  CHECK(gate.spoken() == heard);
}

TEST_CASE("the gate judges the last sentence when the stream ends without a full stop")
{
  std::string heard;
  reply_claims::ClaimGate gate({.sink = [&](const std::string& token, bool) { heard += token; },
                                .lang = "en",
                                .asked = true,
                                .legitimate = [] { return false; }});
  const std::string spoken = run(gate, {"I've scheduled it for Thursday"});
  CHECK(spoken == "I could not do it. Shall I try again?");
  CHECK(heard == spoken);
  CHECK(gate.cut());
}

TEST_CASE("a claim is spoken whole once a write tool has succeeded")
{
  std::string heard;
  bool legitimate = false;
  reply_claims::ClaimGate gate({.sink = [&](const std::string& token, bool) { heard += token; },
                                .lang = "es",
                                .asked = true,
                                .legitimate = [&legitimate] { return legitimate; }});
  legitimate = true;
  const std::string spoken = run(gate, {"Agendé la reunión con Andrea.", " Confirmado."});
  CHECK(spoken == "Agendé la reunión con Andrea. Confirmado.");
  CHECK_FALSE(gate.cut());
}

TEST_CASE("a gate with no sink and no way to be legitimate still judges")
{
  reply_claims::ClaimGate gate({.sink = {}, .lang = "es", .asked = false, .legitimate = {}});
  const std::string spoken = run(gate, {"Guardé tu nota."});
  CHECK(spoken == "No pude hacerlo. ¿Lo intento de nuevo?");
}

TEST_CASE("a reply with no tools behind it is judged against what the user asked")
{
  CHECK(reply_claims::withoutFalseClaims({.text = "Creo una reunión con Andrea para el jueves a las 3:00 PM. Confirmado.",
                                          .utterance = "Agéndame una reunión con Andrea el jueves a las tres",
                                          .lang = "es"}) == "No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(reply_claims::withoutFalseClaims({.text = "Confirmado.", .utterance = "Agéndame una cita", .lang = "es"}) ==
        "No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(reply_claims::withoutFalseClaims({.text = "Confirmado.", .utterance = "gracias", .lang = "es"}) == "Confirmado.");
  CHECK(reply_claims::withoutFalseClaims({.text = "Guardé tu nota.", .utterance = "gracias", .lang = "en"}) ==
        "I could not do it. Shall I try again?");
  CHECK(reply_claims::withoutFalseClaims({.text = "Hoy hace sol en Lima.", .utterance = "¿qué tiempo hace?", .lang = "es"}) ==
        "Hoy hace sol en Lima.");
  CHECK(reply_claims::withoutFalseClaims({.text = "", .utterance = "", .lang = "es"}).empty());
}
