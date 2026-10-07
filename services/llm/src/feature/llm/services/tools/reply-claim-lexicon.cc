#include "reply-claim-lexicon.hxx"

#include <array>

namespace reply_claims
{

namespace
{
using Phrase = std::string_view;

constexpr auto kSpanishPerformed = std::to_array<Phrase>({
    "agende", "programe", "cree", "guarde", "anote", "apunte", "registre", "agregue", "anadi", "puse", "deje",
    "active", "desactive", "apague", "encendi", "cancele", "elimine", "borre", "quite", "complete", "marque",
    "termine", "envie", "avise", "cambie", "movi", "actualice", "reserve", "reprograme", "abri", "mostre",
    "configure", "he agendado", "he programado", "he creado", "he guardado", "he anotado", "he apuntado",
    "he registrado", "he agregado", "he anadido", "he puesto", "he activado", "he desactivado", "he apagado",
    "he cancelado", "he eliminado", "he borrado", "he quitado", "he completado", "he marcado", "he enviado",
    "he avisado", "he cambiado", "he movido", "he actualizado", "he reservado", "quedo agendado", "quedo agendada",
    "quedo anotado", "quedo anotada", "quedo guardado", "quedo guardada", "quedo programado", "quedo programada",
    "quedo creado", "he ajustado", "he subido", "he bajado", "he regulado",
    "he encendido", "he abierto", "he mostrado", "he configurado", "informacion esta guardada",
    "informacion esta anotada", "nota esta guardada", "nota esta anotada", "dato esta guardado", "dato esta anotado",
    "recordatorio esta guardado", "recordatorio esta anotado", "ya esta guardado", "ya esta guardada",
    "ya esta anotado", "ya esta anotada", "claro puedo activar", "claro puedo activarla", "claro puedo activarlo",
    "claro puedo agendar", "claro puedo agendarla", "claro puedo agendarlo", "claro puedo anotar", "claro puedo anotarlo",
    "claro puedo guardar", "claro puedo guardarlo", "claro puedo crear", "claro puedo crearla", "claro puedo crearlo",
    "claro puedo encender", "claro puedo apagar", "claro puedo cancelar", "claro puedo cancelarla", "claro puedo cancelarlo",
    "claro puedo programar", "claro puedo ajustar", "claro puedo ajustarla", "claro puedo ajustarlo",
    "claro que puedo activar", "claro que puedo activarla", "claro que puedo activarlo", "claro que puedo agendar",
    "claro que puedo agendarla", "claro que puedo agendarlo", "claro que puedo anotar", "claro que puedo guardar",
    "claro que puedo crear", "claro que puedo encender", "claro que puedo apagar", "claro que puedo cancelar",
    "claro que puedo programar", "claro que puedo ajustar"});

constexpr auto kSpanishPerformative = std::to_array<Phrase>({
    "creo", "agendo", "programo", "guardo", "anoto", "apunto", "agrego", "anado", "pongo", "activo", "desactivo",
    "apago", "enciendo", "cancelo", "elimino", "borro", "quito", "completo", "marco", "muevo", "reservo", "ajuste", "subi",
    "ajusto", "subo"});

constexpr auto kSpanishDeterminers = std::to_array<Phrase>({"un", "una", "unos", "unas", "el", "la", "los", "las",
                                                      "tu", "tus", "su", "sus", "mi", "este", "esta"});

constexpr auto kSpanishMarkers = std::to_array<Phrase>({
    "listo", "hecho", "confirmado", "agendado", "agendada", "programado", "programada", "guardado", "guardada",
    "anotado", "anotada", "apuntado", "apuntada", "creado", "creada", "registrado", "registrada", "anadido",
    "anadida", "completado", "completada", "ya esta", "ya quedo", "ya fue", "todo listo", "quedo listo",
    "quedo hecho", "ya esta listo", "ya lo tienes", "ya lo tiene", "ya esta hecho"});

constexpr auto kSpanishNegators = std::to_array<Phrase>({"no", "nunca", "jamas", "tampoco", "ni", "sin", "aun no"});

constexpr auto kSpanishHedges = std::to_array<Phrase>({
    "quieres", "quiere", "prefieres", "deseas", "puedo", "podria", "podrias", "gustaria", "quisieras", "voy",
    "vamos", "podemos", "debo", "deberia", "cuando", "mientras", "hasta", "antes", "despues", "luego",
    "intentare", "intentar", "tratare", "necesito", "necesitas", "falta", "debes", "tienes que"});

constexpr auto kSpanishRequests = std::to_array<Phrase>({
    "agendame", "agendar", "agendalo", "agendale", "programame", "programar", "programalo", "crea", "creame",
    "crear", "crealo", "anota", "anotame", "anotar", "anotalo", "apunta", "apuntame", "apuntar", "guarda",
    "guardame", "guardar", "guardalo", "recuerdame", "recordarme", "recuerdales", "activa", "activame",
    "activar", "activalo", "enciende", "apaga", "apagar", "apagalo", "desactiva", "desactivar", "cancela",
    "cancelar", "cancelalo", "borra", "borrar", "borralo", "elimina", "eliminar", "eliminalo", "quita",
    "quitar", "quitalo", "pon", "ponme", "poner", "ponlo", "marca", "marcar", "marcalo", "completa", "completar",
    "anade", "agrega", "agregar", "agregalo", "cambia", "mueve", "reprograma", "reserva", "avisale", "pidele",
    "agendarme", "agendarlo", "programarme", "programarlo", "anotarlo", "anotarme", "guardarlo", "crearlo", "activarlo",
    "apagarlo", "cancelarlo", "borrarlo", "apuntarlo", "ponerlo", "marcarlo", "recordarlo"});

constexpr auto kSpanishLeadingRequests = std::to_array<Phrase>({"agenda", "programa", "avisa", "pide"});

constexpr auto kSpanishOpening = std::to_array<Phrase>({
    "abri", "he abierto", "mostre", "he mostrado", "te muestro", "muestro", "mostrando", "estoy abriendo", "estoy mostrando",
    "abro", "abriendo"});

constexpr auto kSpanishStates = std::to_array<Phrase>({
    "activado", "activada", "activados", "activadas", "desactivado", "desactivada", "desactivados", "desactivadas",
    "encendido", "encendida", "encendidos", "encendidas", "apagado", "apagada", "apagados", "apagadas", "cambiado",
    "cambiada", "cambiados", "cambiadas", "configurado", "configurada", "configurados", "configuradas",
    "ya esta activado", "ya esta activada", "ya esta activados", "ya esta activadas", "ya esta desactivado",
    "ya esta desactivada", "ya esta encendido", "ya esta encendida", "ya esta apagado", "ya esta apagada",
    "ya quedo activado", "ya quedo activada", "quedo activado", "quedo activada", "quedo encendido", "quedo encendida",
    "quedo apagado", "quedo apagada", "quedo desactivado", "quedo desactivada"});

constexpr auto kSpanishCopulas = std::to_array<Phrase>({
    "esta", "estan", "estaba", "estaban", "estara", "estaran", "es", "son", "era", "eran", "sigue", "siguen", "queda",
    "quedan", "permanece", "permanecen"});

constexpr auto kEnglishPerformed = std::to_array<Phrase>({
    "i scheduled", "i have scheduled", "ive scheduled", "i created", "i have created", "ive created", "i added",
    "i have added", "ive added", "i saved", "i have saved", "ive saved", "i set", "i have set", "ive set",
    "i turned on", "i have turned on", "ive turned on", "i turned off", "i have turned off", "ive turned off",
    "i enabled", "i have enabled", "ive enabled", "i disabled", "i cancelled", "i canceled", "i have cancelled",
    "ive cancelled", "ive canceled", "i deleted", "i have deleted", "ive deleted", "i removed", "ive removed",
    "i marked", "ive marked", "i noted", "ive noted", "i booked", "ive booked", "i wrote down", "ive written",
    "im scheduling", "im creating", "im adding", "im saving", "im setting", "im booking", "im turning on",
    "im cancelling", "im canceling", "i adjusted", "i have adjusted", "ive adjusted", "i turned up", "i turned down",
    "ive turned up", "ive turned down", "i raised", "i lowered", "ive raised", "ive lowered", "i opened", "ive opened",
    "i configured", "ive configured", "that information is saved", "its saved for you"});

constexpr std::array<Phrase, 0> kNoPhrases{};

constexpr auto kEnglishOpening = std::to_array<Phrase>({"i opened", "ive opened", "i showed", "ive shown", "im showing", "im opening"});

constexpr auto kEnglishStates = std::to_array<Phrase>({
    "activated", "deactivated", "enabled", "disabled", "switched", "turned on", "turned off", "switched on", "switched off",
    "changed", "configured"});

constexpr auto kEnglishCopulas = std::to_array<Phrase>({"is", "are", "was", "were", "remain", "remains", "stay", "stays", "currently"});

constexpr auto kEnglishMarkers = std::to_array<Phrase>({
    "done", "all set", "confirmed", "scheduled", "created", "saved", "added", "booked", "noted", "its set",
    "its done", "its scheduled", "its saved", "its booked", "its added", "its created", "set up"});

constexpr auto kEnglishNegators = std::to_array<Phrase>({"no", "not", "never", "nor", "cant", "cannot", "couldnt",
                                                   "wont", "didnt", "havent", "hasnt", "wasnt", "wouldnt",
                                                   "unable", "without"});

constexpr auto kEnglishHedges = std::to_array<Phrase>({
    "want", "wanna", "would", "could", "should", "shall", "can", "will", "ill", "going", "gonna", "if",
    "when", "once", "after", "before", "until", "while", "need", "needs", "try", "might"});

constexpr auto kEnglishRequests = std::to_array<Phrase>({
    "schedule", "book", "create", "add", "set", "remind", "save", "note", "enable", "activate", "disable",
    "deactivate", "cancel", "delete", "remove", "mark", "complete", "turn", "switch", "put", "arrange",
    "reschedule", "move", "change", "register", "reserve", "notify", "make", "update", "organize", "plan", "track", "record", "pin", "store"});

constexpr auto kSpanishCalls = std::to_array<Phrase>({
    "te llamare", "te voy a llamar", "te estare llamando", "te marcare", "te hare una llamada", "te llamo a las",
    "te llamo manana", "te llamo hoy", "te llamo en", "te llamo cuando", "te llamare a las"});

constexpr auto kSpanishCallOffers = std::to_array<Phrase>({
    "si", "quieres", "quiere", "prefieres", "deseas", "puedo", "podria", "podrias", "gustaria", "quisieras"});

constexpr auto kEnglishCalls = std::to_array<Phrase>({
    "i will call you", "ill call you", "i am going to call you", "im going to call you", "i will ring you", "ill ring you",
    "i will phone you", "ill phone you", "i will give you a call", "ill give you a call", "i will be calling you",
    "ill be calling you"});

constexpr auto kEnglishCallOffers = std::to_array<Phrase>({
    "if", "want", "wants", "would", "could", "can", "shall", "should", "may", "might"});

constexpr std::string_view kSpanishHonest = "No pude hacerlo. ¿Lo intento de nuevo?";
constexpr std::string_view kEnglishHonest = "I could not do it. Shall I try again?";

constexpr std::string_view kSpanishNudge =
    "Todavía no has usado ninguna herramienta con éxito. Si el usuario pidió algo (agendar, guardar, activar, "
    "cancelar...), llama ahora a su herramienta; si no puedes, díselo. No digas que lo hiciste.";
constexpr std::string_view kEnglishNudge =
    "You have not used any tool successfully yet. If the user asked for something (to schedule, save, enable, "
    "cancel...), call its tool now; if you cannot, say so. Do not say you did it.";

constexpr std::array<Lexicon, 2> kLexicons{{
    {.language = "es",
     .performed = kSpanishPerformed,
     .performative = kSpanishPerformative,
     .determiners = kSpanishDeterminers,
     .markers = kSpanishMarkers,
     .negators = kSpanishNegators,
     .hedges = kSpanishHedges,
     .requests = kSpanishRequests,
     .leadingRequests = kSpanishLeadingRequests,
     .opening = kSpanishOpening,
     .states = kSpanishStates,
     .copulas = kSpanishCopulas,
     .calls = kSpanishCalls,
     .callOffers = kSpanishCallOffers,
     .honest = kSpanishHonest,
     .nudge = kSpanishNudge},
    {.language = "en",
     .performed = kEnglishPerformed,
     .performative = kNoPhrases,
     .determiners = kNoPhrases,
     .markers = kEnglishMarkers,
     .negators = kEnglishNegators,
     .hedges = kEnglishHedges,
     .requests = kEnglishRequests,
     .leadingRequests = kNoPhrases,
     .opening = kEnglishOpening,
     .states = kEnglishStates,
     .copulas = kEnglishCopulas,
     .calls = kEnglishCalls,
     .callOffers = kEnglishCallOffers,
     .honest = kEnglishHonest,
     .nudge = kEnglishNudge},
}};
}

std::span<const Lexicon> lexicons()
{
  return kLexicons;
}

const Lexicon& lexiconFor(std::string_view language)
{
  return language == "en" ? kLexicons[1] : kLexicons[0];
}

}
