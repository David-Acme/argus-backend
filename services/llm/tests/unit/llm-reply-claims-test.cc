#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/claim-check.hxx>
#include <feature/llm/services/tools/reply-claims.hxx>

#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::string joined(const std::vector<std::string>& tokens)
{
  std::string out;
  for (const std::string& token : tokens)
    out += token;
  return out;
}

struct StreamScript
{
  const std::vector<std::string>& tokens;
  std::chrono::milliseconds perToken{2};
};

struct ReleaseTiming
{
  std::string heard;
  std::size_t releasedAt{0};
  int64_t releasedMs{0};
  std::size_t stripped{0};
};

ReleaseTiming pump(const StreamScript& script)
{
  ReleaseTiming out;
  std::size_t fed = 0;
  std::optional<std::chrono::steady_clock::time_point> released;
  reply_claims::OfferStripGate gate({.sink =
                                         [&](const std::string& token, bool) {
                                           if (!token.empty() && !released) {
                                             released = std::chrono::steady_clock::now();
                                             out.releasedAt = fed;
                                           }
                                           out.heard += token;
                                         },
                                     .lang = "es",
                                     .asked = false});
  const TokenCallback callback = gate.callback();
  const auto started = std::chrono::steady_clock::now();
  for (const std::string& token : script.tokens) {
    ++fed;
    callback(token, false);
    std::this_thread::sleep_for(script.perToken);
  }
  callback("", true);
  out.stripped = gate.stripped();
  if (released)
    out.releasedMs = std::chrono::duration_cast<std::chrono::milliseconds>(*released - started).count();
  return out;
}

bool claims(const std::string& text, bool asked = false)
{
  return reply_claims::claimsDone({.text = text, .asked = asked});
}

bool callClaim(const std::string& text, const std::string& lang = "")
{
  return reply_claims::claimsCall({.text = text, .lang = lang});
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

std::string runStrip(reply_claims::OfferStripGate& gate, const std::vector<std::string>& tokens)
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

TEST_CASE("Peruvian and colloquial Spanish completions are claims")
{
  CHECK(claims("Ya pues, jefe, quedó agendado para el jueves."));
  CHECK(claims("Ya te lo dejé anotado, causa."));
  CHECK(claims("Listo pues, ya lo programé."));
  CHECK(claims("Dale, ya lo guardé en tu lista."));
  CHECK(claims("Ya está pues, anotado.", true));
  CHECK(claims("Ya fue, jefe.", true));
  CHECK(claims("Hecho, hermano.", true));
  CHECK(claims("Todo listo, ya quedó.", true));
  CHECK(claims("Ya te creé la tarea, no te preocupes."));
  CHECK(claims("Te avisé al dueño de la casa."));
  CHECK_FALSE(claims("Ya pues, dime la hora y lo agendo.", true));
  CHECK_FALSE(claims("Ya pues, no pude anotarlo.", true));
  CHECK_FALSE(claims("¿Ya está listo el informe?", true));
}

TEST_CASE("English completions and their hedged forms")
{
  CHECK(claims("Got it. I've added the meeting to your agenda."));
  CHECK(claims("Sure thing, I booked it for Thursday."));
  CHECK(claims("I've enabled the productivity module."));
  CHECK(claims("Alright, I created the project for you."));
  CHECK(claims("It's scheduled.", true));
  CHECK(claims("Done! Your reminder is set.", true));
  CHECK(claims("Perfect, all set.", true));
  CHECK_FALSE(claims("I can't schedule that because the module is off.", true));
  CHECK_FALSE(claims("I wasn't able to save it.", true));
  CHECK_FALSE(claims("Should I schedule it for Thursday?", true));
  CHECK_FALSE(claims("I'll schedule it as soon as you confirm the time.", true));
  CHECK_FALSE(claims("When it's scheduled I will tell you.", true));
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

TEST_CASE("a question about what Argus can do is recognized in both languages")
{
  CHECK(reply_claims::asksCapabilities("¿Qué puedes hacer?"));
  CHECK(reply_claims::asksCapabilities("Argus, ¿qué sabes hacer?"));
  CHECK(reply_claims::asksCapabilities("¿En qué puedes ayudar?"));
  CHECK(reply_claims::asksCapabilities("What can you do?"));
  CHECK(reply_claims::asksCapabilities("How can you help me?"));
  CHECK_FALSE(reply_claims::asksCapabilities("¿Qué tengo en mi agenda?"));
  CHECK_FALSE(reply_claims::asksCapabilities("¿Me puedes ayudar con la cena?"));
  CHECK_FALSE(reply_claims::asksCapabilities("Gracias"));
}

TEST_CASE("the honest reply comes in the user's language")
{
  CHECK(reply_claims::honest("es") == "No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(reply_claims::honest("en") == "I could not do it. Shall I try again?");
  CHECK(reply_claims::honest("fr") == reply_claims::honest("es"));
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
  CHECK(reply_claims::withoutFalseClaims({.text = "I saved your note.", .utterance = "thanks", .lang = "en"}) ==
        "I could not do it. Shall I try again?");
  CHECK(reply_claims::withoutFalseClaims({.text = "Hoy hace sol en Lima.", .utterance = "¿qué tiempo hace?", .lang = "es"}) ==
        "Hoy hace sol en Lima.");
  CHECK(reply_claims::withoutFalseClaims({.text = "", .utterance = "", .lang = "es"}).empty());
}

TEST_CASE("an adjustment, a saved note and a promise to act now are claims, and the same words as a question or a plan are not")
{
  CHECK(claims("¡Hola! He ajustado la calefacción para que esté un par de grados más cálida."));
  CHECK(claims("Ajusté la calefacción a veintidós grados."));
  CHECK(claims("Subí la calefacción un par de grados."));
  CHECK(claims("He bajado las luces del salón."));
  CHECK(claims("I adjusted the heating a couple of degrees."));
  CHECK(claims("I've turned up the heating."));
  CHECK(claims("El sistema registra que el perro no puede comer chocolate. Esa información está guardada para tu seguridad."));
  CHECK(claims("Ya está guardado."));
  CHECK(claims("Claro, puedo activar la agenda ahora."));
  CHECK(claims("Claro que puedo agendarlo ahora mismo."));
  CHECK_FALSE(claims("¿Quieres que ajuste la calefacción?"));
  CHECK_FALSE(claims("No he ajustado la calefacción todavía."));
  CHECK_FALSE(claims("Cuando ajuste la calefacción te aviso."));
  CHECK_FALSE(claims("Puedo ajustar la calefacción si quieres."));
  CHECK_FALSE(claims("Claro, puedo ayudarte con eso."));
  CHECK_FALSE(claims("Estoy listo para ayudarte."));
  CHECK_FALSE(claims("Tu llave está en el cajón."));
  CHECK_FALSE(claims("No hay eventos programados este sábado."));
}

namespace
{
bool shown(const std::string& text)
{
  return reply_claims::claimsDone({.text = text, .asked = true, .appOnly = true});
}
}

TEST_CASE("after a screen was only opened, a claim of a state change is a claim and a description or the opening itself is not")
{
  CHECK(shown("Estoy listo para ayudarte, aquí tienes las cámaras activadas."));
  CHECK(shown("Activé las cámaras y las abrí."));
  CHECK(shown("Encendí la sala y te la muestro."));
  CHECK(shown("Apagué la alarma."));
  CHECK(shown("Desactivé la vigilancia."));
  CHECK(shown("Cambié el modo a noche."));
  CHECK(shown("Puse la vigilancia en modo noche."));
  CHECK(shown("Aquí tienes las luces apagadas."));
  CHECK(shown("Listo pues, ya quedó activado."));
  CHECK(shown("Ya está activada la alarma pe."));
  CHECK(shown("I turned on the cameras and opened them."));
  CHECK(shown("I've enabled the cameras."));
  CHECK(shown("Here are your cameras, activated."));
  CHECK(shown("I switched the lights off."));

  CHECK_FALSE(shown("Abrí las cámaras."));
  CHECK_FALSE(shown("Te muestro las cámaras."));
  CHECK_FALSE(shown("Estoy mostrando la cámara del garaje."));
  CHECK_FALSE(shown("Listo, aquí tienes las cámaras."));
  CHECK_FALSE(shown("Ya te abrí las cámaras pe."));
  CHECK_FALSE(shown("Ahí tienes tus cámaras, causa."));
  CHECK_FALSE(shown("Las cámaras que están activas son el garaje y la puerta."));
  CHECK_FALSE(shown("Tus cámaras están activadas."));
  CHECK_FALSE(shown("Las cámaras siguen encendidas."));
  CHECK_FALSE(shown("La alarma está desactivada desde anoche."));
  CHECK_FALSE(shown("¿Quieres que active las cámaras?"));
  CHECK_FALSE(shown("No activé nada todavía."));
  CHECK_FALSE(shown("Voy a activarlas si quieres."));
  CHECK_FALSE(shown("I opened the notifications."));
  CHECK_FALSE(shown("Here are your cameras."));
  CHECK_FALSE(shown("Your cameras are online."));
  CHECK_FALSE(shown("Your cameras are enabled and the garage one is on."));
  CHECK_FALSE(shown("The alarm remains turned off."));
  CHECK_FALSE(shown("I'm showing the garage camera."));
}

TEST_CASE("the same state words are claims when nothing ran and the user asked, and descriptions never are")
{
  CHECK(claims("Aquí tienes las cámaras activadas.", true));
  CHECK(claims("Ya está activado el modo noche.", true));
  CHECK_FALSE(claims("Aquí tienes las cámaras activadas.", false));
  CHECK_FALSE(claims("Tus cámaras están activadas.", true));
  CHECK_FALSE(claims("The cameras are enabled.", true));
  CHECK_FALSE(claims("Las luces siguen apagadas.", true));
}

TEST_CASE("a turn's language reads the reply with that language's phrases only, so an English adjective is not a Spanish verb")
{
  const auto english = [](const std::string& text, bool appOnly) {
    return reply_claims::claimsDone({.text = text, .asked = true, .appOnly = appOnly, .lang = "en"});
  };
  const auto spanish = [](const std::string& text, bool appOnly) {
    return reply_claims::claimsDone({.text = text, .asked = true, .appOnly = appOnly, .lang = "es"});
  };
  CHECK_FALSE(english("The screens show the garage, door, and yard cameras active.", true));
  CHECK_FALSE(english("Your cameras are active.", false));
  CHECK(claims("Your cameras are active.", true));
  CHECK(english("I've scheduled the meeting with Andrea.", false));
  CHECK(english("I turned on the cameras.", true));
  CHECK(spanish("Activé las cámaras.", true));
  CHECK(spanish("Agendé la reunión con Andrea.", false));
  CHECK(reply_claims::claimsDone({.text = "Agendé la reunión.", .asked = false, .appOnly = false, .lang = ""}));
  CHECK_FALSE(english("That is quite a lot of events, and the setup is complete.", false));
  CHECK_FALSE(english("Your configuration is complete and I can reserve a table if you like.", false));
}

TEST_CASE("a promise to call the user is a claim in Spanish, Peruvian Spanish and English")
{
  CHECK(callClaim("Listo, te llamaré a las 3 de la tarde.", "es"));
  CHECK(callClaim("Te llamaré el jueves 8 a las 3 de la tarde."));
  CHECK(callClaim("Ya pues, te llamo mañana a las cinco."));
  CHECK(callClaim("Te voy a llamar a las nueve."));
  CHECK(callClaim("Te estaré llamando a esa hora."));
  CHECK(callClaim("I will call you on Thursday at 3 PM.", "en"));
  CHECK(callClaim("Sure, I'll call you tomorrow."));
  CHECK(callClaim("I'm going to call you at five."));
  CHECK(callClaim("I will give you a call at nine."));
}

TEST_CASE("an offer, a question or a negation about a call is not a claim")
{
  CHECK_FALSE(callClaim("¿Quieres que te llame mañana?"));
  CHECK_FALSE(callClaim("Si quieres, te llamaré a las cinco."));
  CHECK_FALSE(callClaim("No te llamaré hasta que me lo pidas."));
  CHECK_FALSE(callClaim("Puedo llamarte si lo prefieres."));
  CHECK_FALSE(callClaim("Shall I call you tomorrow?"));
  CHECK_FALSE(callClaim("If you want, I will call you at five."));
  CHECK_FALSE(callClaim("I will not call you."));
  CHECK_FALSE(callClaim("I can call you if you like."));
  CHECK_FALSE(callClaim("Te llamaré", "en"));
  CHECK_FALSE(callClaim("I will call you", "es"));
  CHECK_FALSE(callClaim("Quedó en tus recordatorios para el jueves 8 a las 3 de la tarde."));
  CHECK_FALSE(callClaim("Lo guardé en tus recordatorios, pero no pude programar la llamada."));
}

TEST_CASE("a call claim without a confirmed schedule is a false completion even after a write that worked")
{
  const TurnState unconfirmed{.asked = true, .wrote = true, .called = false, .lang = "es"};
  CHECK(claimedWithoutTool("Listo, te llamaré a las 3.", unconfirmed));
  CHECK(claimedWithoutTool("Listo, lo anoté.", {.asked = true, .wrote = false, .lang = "es"}));
  CHECK_FALSE(claimedWithoutTool("Listo, lo anoté.", unconfirmed));
  const TurnState confirmed{.asked = true, .wrote = true, .called = true, .lang = "es"};
  CHECK_FALSE(claimedWithoutTool("Listo, te llamaré a las 3.", confirmed));
  CHECK(claimedWithoutTool("I will call you at 3.", {.asked = true, .wrote = true, .called = false, .lang = "en"}));
  CHECK_FALSE(claimedWithoutTool("I will call you at 3.", {.asked = true, .wrote = true, .called = true, .lang = "en"}));
  CHECK(claimedWithoutTool("Te llamaré mañana.", {.asked = false, .wrote = false, .called = false, .lang = "es"}));
}

TEST_CASE("the gate cuts a call promise until the schedule is confirmed, and lets it through once it is")
{
  std::string heard;
  reply_claims::ClaimGate unconfirmed({.sink = [&](const std::string& token, bool) { heard += token; },
                                       .lang = "es",
                                       .asked = true,
                                       .legitimate = [] { return true; },
                                       .appOnly = false,
                                       .callsConfirmed = false});
  CHECK(run(unconfirmed, {"Lo anoté. ", "Te llamaré a las tres."}) == "Lo anoté. No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(unconfirmed.cut());

  std::string confirmedHeard;
  reply_claims::ClaimGate confirmed({.sink = [&](const std::string& token, bool) { confirmedHeard += token; },
                                     .lang = "es",
                                     .asked = true,
                                     .legitimate = [] { return true; },
                                     .appOnly = false,
                                     .callsConfirmed = true});
  CHECK(run(confirmed, {"Lo anoté. ", "Te llamaré a las tres."}) == "Lo anoté. Te llamaré a las tres.");
  CHECK_FALSE(confirmed.cut());
}

TEST_CASE("a plain reply with no tools never promises a call")
{
  CHECK(reply_claims::withoutFalseClaims({.text = "Claro, te llamaré a las cinco.", .utterance = "llámame a las cinco", .lang = "es"}) ==
        "No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(reply_claims::withoutFalseClaims({.text = "Hace sol hoy.", .utterance = "qué tiempo hace", .lang = "es"}) == "Hace sol hoy.");
}

TEST_CASE("a generic help offer is caught in either language")
{
  CHECK(reply_claims::genericOffer({.text = "¡Hola! Estoy bien, gracias. ¿En qué puedo ayudarte hoy?", .lang = "es"}));
  CHECK(reply_claims::genericOffer({.text = "Estoy bien, ¿en qué te puedo ayudar?", .lang = "es"}));
  CHECK(reply_claims::genericOffer({.text = "Claro, ¿en qué puedo servirte?", .lang = "es"}));
  CHECK(reply_claims::genericOffer({.text = "¿En qué puedo asistirte?", .lang = "es"}));
  CHECK(reply_claims::genericOffer({.text = "Dime, ¿qué puedo hacer por ti?", .lang = "es"}));
  CHECK(reply_claims::genericOffer({.text = "¿Necesitas algo más?", .lang = "es"}));
  CHECK(reply_claims::genericOffer({.text = "Hello! How can I help you today?", .lang = "en"}));
  CHECK(reply_claims::genericOffer({.text = "Is there something I can do? What can I do for you?", .lang = "en"}));
  CHECK(reply_claims::genericOffer({.text = "Do you need anything else?", .lang = "en"}));
  CHECK_FALSE(reply_claims::genericOffer({.text = "¿Te muestro la cámara?", .lang = "es"}));
  CHECK_FALSE(reply_claims::genericOffer({.text = "Claro, te la muestro en la app.", .lang = "es"}));
  CHECK_FALSE(reply_claims::genericOffer({.text = "I can show it to you in the app.", .lang = "en"}));
}

TEST_CASE("a plain reply keeps its final sentence when a trailing generic offer is dropped")
{
  const reply_claims::OfferContext unasked{.lang = "es", .asked = false};
  const reply_claims::OfferContext asked{.lang = "es", .asked = true};

  const reply_claims::StrippedReply dropped =
      reply_claims::withoutTrailingOffer("Hoy es miércoles 7 de octubre. ¿Necesitas algo más?", unasked);
  CHECK(dropped.text == "Hoy es miércoles 7 de octubre.");
  CHECK(dropped.stripped);

  const reply_claims::StrippedReply kept =
      reply_claims::withoutTrailingOffer("Hoy es miércoles 7 de octubre. ¿Necesitas algo más?", asked);
  CHECK(kept.text == "Hoy es miércoles 7 de octubre. ¿Necesitas algo más?");
  CHECK_FALSE(kept.stripped);

  const reply_claims::StrippedReply alone = reply_claims::withoutTrailingOffer("¿Necesitas algo más?", unasked);
  CHECK(alone.text == "¿Necesitas algo más?");
  CHECK_FALSE(alone.stripped);

  const reply_claims::StrippedReply plain = reply_claims::withoutTrailingOffer("Mañana llueve. Lleva paraguas.", unasked);
  CHECK(plain.text == "Mañana llueve. Lleva paraguas.");
  CHECK_FALSE(plain.stripped);

  const reply_claims::OfferContext english{.lang = "en", .asked = false};
  CHECK(reply_claims::withoutTrailingOffer("It is 6 pm. Do you need anything else?", english).text == "It is 6 pm.");
}

TEST_CASE("a sentence that only ends in an offer keeps the content that comes before it")
{
  const reply_claims::OfferContext spanish{.lang = "es", .asked = false};
  const reply_claims::StrippedReply comma =
      reply_claims::withoutTrailingOffer("Listo. Apunté la leche, ¿necesitas algo más?", spanish);
  CHECK(comma.text == "Listo. Apunté la leche, ¿necesitas algo más?");
  CHECK_FALSE(comma.stripped);

  const reply_claims::StrippedReply reminder =
      reply_claims::withoutTrailingOffer("Hecho. Recuerda que el lunes necesitas algo de efectivo.", spanish);
  CHECK(reminder.text == "Hecho. Recuerda que el lunes necesitas algo de efectivo.");
  CHECK_FALSE(reminder.stripped);

  const reply_claims::OfferContext english{.lang = "en", .asked = false};
  const reply_claims::StrippedReply added =
      reply_claims::withoutTrailingOffer("Done. I added the milk, do you need anything else?", english);
  CHECK(added.text == "Done. I added the milk, do you need anything else?");
  CHECK_FALSE(added.stripped);

  const reply_claims::StrippedReply cash =
      reply_claims::withoutTrailingOffer("Done. Remember you need some cash on Monday, do you need anything else?", english);
  CHECK(cash.text == "Done. Remember you need some cash on Monday, do you need anything else?");
  CHECK_FALSE(cash.stripped);

  std::string heard;
  reply_claims::OfferStripGate gate({.sink = [&heard](const std::string& token, bool) { heard += token; },
                                     .lang = "es",
                                     .asked = false});
  CHECK(runStrip(gate, {"Listo. ", "Apunté la leche, ", "¿necesitas algo más?"}) ==
        "Listo. Apunté la leche, ¿necesitas algo más?");
  CHECK(gate.stripped() == 0);
  CHECK(heard == "Listo. Apunté la leche, ¿necesitas algo más?");
}

TEST_CASE("an offer padded with an interjection or a trailing particle is still the offer")
{
  const reply_claims::OfferContext spanish{.lang = "es", .asked = false};
  const reply_claims::StrippedReply padded =
      reply_claims::withoutTrailingOffer("Hoy es miércoles. Oye, ¿necesitas algo más?", spanish);
  CHECK(padded.text == "Hoy es miércoles.");
  CHECK(padded.stripped);

  const reply_claims::StrippedReply paddedToday =
      reply_claims::withoutTrailingOffer("Hola, David. Todo está bien. ¿En qué puedo ayudarte hoy?", spanish);
  CHECK(paddedToday.text == "Hola, David. Todo está bien.");
  CHECK(paddedToday.stripped);

  const reply_claims::StrippedReply paddedNow =
      reply_claims::withoutTrailingOffer("Hoy es miércoles. ¿Necesitas algo ahora?", spanish);
  CHECK(paddedNow.stripped);

  const reply_claims::OfferContext english{.lang = "en", .asked = false};
  const reply_claims::StrippedReply paddedEn =
      reply_claims::withoutTrailingOffer("It is 6 pm. Hey, do you need anything else?", english);
  CHECK(paddedEn.text == "It is 6 pm.");
  CHECK(paddedEn.stripped);

  const reply_claims::StrippedReply paddedFurther =
      reply_claims::withoutTrailingOffer("Hi David, it's good to connect. How can I assist you further?", english);
  CHECK(paddedFurther.text == "Hi David, it's good to connect.");
  CHECK(paddedFurther.stripped);

  std::string heard;
  reply_claims::OfferStripGate gate({.sink = [&heard](const std::string& token, bool) { heard += token; },
                                     .lang = "es",
                                     .asked = false});
  CHECK(runStrip(gate, {"Hoy es miércoles. ", "Oye, ", "¿Necesitas algo más?"}) == "Hoy es miércoles.");
  CHECK(gate.stripped() == 1);
  CHECK(heard == "Hoy es miércoles.");
}

TEST_CASE("an offer keeps its sentence when a content word follows it")
{
  const reply_claims::OfferContext spanish{.lang = "es", .asked = false};
  const reply_claims::StrippedReply cash =
      reply_claims::withoutTrailingOffer("Hecho. Necesitas algo de efectivo.", spanish);
  CHECK(cash.text == "Hecho. Necesitas algo de efectivo.");
  CHECK_FALSE(cash.stripped);

  const reply_claims::StrippedReply camera =
      reply_claims::withoutTrailingOffer("Hoy es miércoles. ¿En qué puedo ayudarte con la cámara?", spanish);
  CHECK(camera.text == "Hoy es miércoles. ¿En qué puedo ayudarte con la cámara?");
  CHECK_FALSE(camera.stripped);

  const reply_claims::OfferContext english{.lang = "en", .asked = false};
  const reply_claims::StrippedReply trip =
      reply_claims::withoutTrailingOffer("It is 6 pm. Do you need anything for the trip?", english);
  CHECK(trip.text == "It is 6 pm. Do you need anything for the trip?");
  CHECK_FALSE(trip.stripped);
}

TEST_CASE("the streaming gate keeps the blank line between two sentences")
{
  std::string heard;
  reply_claims::OfferStripGate gate({.sink = [&heard](const std::string& token, bool) { heard += token; },
                                     .lang = "es",
                                     .asked = false});
  CHECK(runStrip(gate, {"Hoy es miércoles.\n", "\n", "Mañana llueve."}) == "Hoy es miércoles.\n\nMañana llueve.");
  CHECK(gate.stripped() == 0);

  std::string offerHeard;
  reply_claims::OfferStripGate offer({.sink = [&offerHeard](const std::string& token, bool) { offerHeard += token; },
                                      .lang = "es",
                                      .asked = false});
  CHECK(runStrip(offer, {"Hoy es miércoles.\n", "\n", "¿Necesitas algo más?"}) == "Hoy es miércoles.");
  CHECK(offer.stripped() == 1);
}

TEST_CASE("a sentence that cannot be a trailing offer reaches the sink the moment it completes")
{
  std::vector<std::string> quiet{"Hoy es miércoles. "};
  for (int index = 0; index < 40; ++index)
    quiet.emplace_back("palabra ");
  quiet.emplace_back("¿Qué te preocupa?");
  std::vector<std::string> offer = quiet;
  offer.back() = "¿Qué te preocupa? ";
  offer.emplace_back("¿Necesitas algo más?");

  const ReleaseTiming quietTail = pump({.tokens = quiet});
  const ReleaseTiming offerTail = pump({.tokens = offer});
  std::cout << std::format("strip gate: first sentence out after {} tokens at {} ms of {} queued behind it; "
                           "offer tail after {} tokens at {} ms, {} dropped\n",
                           quietTail.releasedAt, quietTail.releasedMs, quiet.size(),
                           offerTail.releasedAt, offerTail.releasedMs, offerTail.stripped);
  CHECK(quietTail.releasedAt == 1);
  CHECK(quietTail.heard == joined(quiet));
  CHECK(offerTail.releasedAt == 1);
  CHECK(offerTail.heard == joined(quiet));
  CHECK(offerTail.stripped == 1);
}

TEST_CASE("the streaming gate drops the same trailing offer and never the whole reply")
{
  std::string heard;
  reply_claims::OfferStripGate unasked({.sink = [&heard](const std::string& token, bool) { heard += token; },
                                        .lang = "es",
                                        .asked = false});
  CHECK(runStrip(unasked, {"Hoy es miércoles 7 de octubre. ", "¿Necesitas ", "algo más?"}) == "Hoy es miércoles 7 de octubre.");
  CHECK(unasked.stripped() == 1);
  CHECK(heard == "Hoy es miércoles 7 de octubre.");

  std::string heardAsked;
  reply_claims::OfferStripGate asked({.sink = [&heardAsked](const std::string& token, bool) { heardAsked += token; },
                                      .lang = "es",
                                      .asked = true});
  CHECK(runStrip(asked, {"Hoy es miércoles. ", "¿Necesitas algo más?"}) == "Hoy es miércoles. ¿Necesitas algo más?");
  CHECK(asked.stripped() == 0);
  CHECK(heardAsked == "Hoy es miércoles. ¿Necesitas algo más?");

  std::string heardAlone;
  reply_claims::OfferStripGate alone({.sink = [&heardAlone](const std::string& token, bool) { heardAlone += token; },
                                      .lang = "es",
                                      .asked = false});
  CHECK(runStrip(alone, {"¿Necesitas algo más?"}) == "¿Necesitas algo más?");
  CHECK(alone.stripped() == 0);
  CHECK(heardAlone == "¿Necesitas algo más?");
}
