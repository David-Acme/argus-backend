#include "module-command-lexicon.hxx"

#include <array>

namespace module_command
{

namespace
{
using Phrase = std::string_view;

constexpr std::array<Phrase, 0> kNothing{};

constexpr auto kEsCreateVerb = std::to_array<Phrase>({
    "crea", "creame", "crear", "anade", "anademe", "anadir", "agrega", "agregame", "agregar", "anota", "anotame",
    "anotar", "apunta", "apuntame", "apuntar", "pon", "ponme", "poner", "programa", "programame", "programar",
    "reserva", "reservame", "reservar", "agendame", "agendar", "agendalo", "agendarme", "agendarlo", "registra",
    "registrame", "registrar", "genera", "haz", "hazme", "mete", "metele"});
constexpr auto kEsStrongCreate = std::to_array<Phrase>({"agendame", "agendar", "agendalo", "agendarme", "agendarlo",
                                                         "programame", "programarme", "reserva", "reservame"});
constexpr auto kEsLeadAgenda = std::to_array<Phrase>({"agenda un", "agenda una", "agenda el", "agenda la", "agenda los",
                                                       "agenda las", "agenda me", "agenda para", "agenda manana",
                                                       "agenda hoy", "agenda otra"});
constexpr auto kEsEventNoun = std::to_array<Phrase>({
    "reunion", "reuniones", "cita", "citas", "evento", "eventos", "junta", "juntas", "llamada", "llamadas",
    "videollamada", "videollamadas", "entrevista", "consulta"});
constexpr auto kEsCalendarNoun = std::to_array<Phrase>({"agenda", "calendario"});
constexpr auto kEsTimeMark = std::to_array<Phrase>({
    "lunes", "martes", "miercoles", "jueves", "viernes", "sabado", "domingo", "manana", "hoy", "a las", "a la",
    "para el", "pasado manana", "proxima", "proximo"});
constexpr auto kEsCancelVerb = std::to_array<Phrase>({
    "cancela", "cancelar", "cancelalo", "cancelala", "borra", "borrar", "borrala", "borralo", "elimina", "eliminar",
    "eliminalo", "eliminala", "anula", "anular", "anulala", "anulalo", "quita", "quitar", "saca", "sacar"});
constexpr auto kEsQueryMarker = std::to_array<Phrase>({
    "que", "cual", "cuales", "dime", "leeme", "lee", "muestrame", "ensename", "lista", "listame", "cuantos",
    "cuantas", "dame", "cuando", "me quedan", "me faltan", "en que"});
constexpr auto kEsFreeMarker = std::to_array<Phrase>({"estoy libre", "estoy ocupado", "estoy ocupada", "tengo libre"});
constexpr auto kEsHaveScheduled = std::to_array<Phrase>({
    "tengo algo programado", "tengo algo agendado", "tengo agendado", "tengo programado", "tengo planeado",
    "algo programado", "algo agendado", "lo que tengo agendado", "lo que tengo programado", "tengo reuniones",
    "tengo citas", "tengo eventos", "tengo alguna reunion", "tengo alguna cita", "tengo algun evento"});
constexpr auto kEsTaskNoun = std::to_array<Phrase>({"tarea", "tareas", "pendiente", "pendientes", "quehaceres"});
constexpr auto kEsNewMark = std::to_array<Phrase>({"nueva tarea", "nuevo pendiente", "nuevo proyecto", "proyecto nuevo"});
constexpr auto kEsCompleteVerb = std::to_array<Phrase>({"completa", "completar", "termina", "terminar", "finaliza"});
constexpr auto kEsCompleteStrong = std::to_array<Phrase>({
    "tacha", "tachalo", "tachala", "tachar", "da por cumplida", "da por cumplido", "da por hecha", "da por hecho",
    "da por terminada", "da por terminado", "da por completada", "completa lo de"});
constexpr auto kEsMarkVerb = std::to_array<Phrase>({"marca", "marcar", "marcala", "marcalo"});
constexpr auto kEsDoneMark = std::to_array<Phrase>({
    "como hecha", "como hecho", "como terminada", "como terminado", "como completada", "como completado",
    "como lista", "como listo", "como cumplida", "como cumplido", "hecha", "terminada", "completada"});
constexpr auto kEsProjectNoun = std::to_array<Phrase>({"proyecto", "proyectos"});
constexpr auto kEsProjectVerb = std::to_array<Phrase>({"crea", "creame", "crear", "abre", "empieza", "empezar",
                                                        "inicia", "iniciar", "quiero", "necesito", "arma", "armar"});
constexpr auto kEsScreenWord = std::to_array<Phrase>({"seccion", "pantalla", "ve a", "vamos a", "llevame"});
constexpr auto kEsModuleNoun = std::to_array<Phrase>({"modulo", "modulos"});
constexpr auto kEsModulePlural = std::to_array<Phrase>({"modulos"});
constexpr auto kEsBookingObject = std::to_array<Phrase>({"mesa", "habitacion", "hotel", "vuelo", "boleto", "boletos", "pasaje", "pasajes",
                                                           "taxi", "restaurante", "cancha", "entrada", "entradas", "hospedaje"});
constexpr auto kEsProjectVeto = std::to_array<Phrase>({"de ley", "de ley", "proyecto de ley", "proyectos de ley"});
constexpr auto kEsEnableVerb = std::to_array<Phrase>({
    "activa", "activame", "activalo", "activala", "activar", "activarla", "activarlo", "activen", "active", "enciende",
    "encender", "prende", "prender", "habilita", "habilitar", "instala", "instalar", "enciendela", "enciendelo",
    "habilitame", "instalame", "prendeme", "activame"});
constexpr auto kEsDisableVerb = std::to_array<Phrase>({
    "apaga", "apagar", "apagala", "apagalo", "desactiva", "desactivar", "desactivala", "desactivalo", "deshabilita",
    "deshabilitar", "dejar de usar", "deja de usar", "desactivame", "apagame", "deshabilitame"});
constexpr auto kEsPurgeVerb = std::to_array<Phrase>({"borra", "borrar", "borralo", "elimina", "eliminar", "eliminalo",
                                                      "suprime", "limpia", "limpiar"});
constexpr auto kEsDataNoun = std::to_array<Phrase>({"datos", "informacion", "lo que guardo", "lo que guardaste",
                                                     "todo lo que guardo", "historial"});
constexpr auto kEsRequestVerb = std::to_array<Phrase>({"pide", "pidele", "pideselo", "pedir", "solicita", "solicitar",
                                                        "dile", "avisale", "dile al"});
constexpr auto kEsOwnerNoun = std::to_array<Phrase>({"dueno", "duena", "administrador", "administradora"});
constexpr auto kEsExplainMarker = std::to_array<Phrase>({
    "explica", "explicame", "explicar", "cuentame", "para que sirve", "que hace", "que incluye",
    "que puedo hacer", "que ofrece", "que trae", "describe", "describeme", "como funciona", "para que me sirve",
    "de que trata"});
constexpr auto kEsExplainGeneric = std::to_array<Phrase>({"que es", "que son"});
constexpr auto kEsReminderNoun = std::to_array<Phrase>({"recordatorio", "recordatorios", "recuerdame", "recuerdales", "avisame", "dime cuando"});
constexpr auto kEsDecline = std::to_array<Phrase>({"no", "mejor no", "dejalo", "olvidalo", "no gracias", "ni hablar", "todavia no", "ahora no"});
constexpr auto kEsFiller = std::to_array<Phrase>({"eh", "em", "a", "ver", "oye", "argus", "hey", "ok", "por", "favor",
                                                   "porfa", "bueno", "mira", "ya", "esta", "pues", "este", "pe", "y",
                                                   "entonces", "ahorita", "ahora", "dale"});

constexpr auto kEnCreateVerb = std::to_array<Phrase>({"schedule", "book", "add", "create", "put", "set up", "make",
                                                       "arrange", "new", "note", "log", "register", "set"});
constexpr auto kEnStrongCreate = std::to_array<Phrase>({"schedule", "book", "set up", "arrange", "reschedule"});
constexpr auto kEnEventNoun = std::to_array<Phrase>({"meeting", "meetings", "appointment", "appointments", "event",
                                                      "events", "call", "calls", "video call", "interview"});
constexpr auto kEnCalendarNoun = std::to_array<Phrase>({"calendar", "agenda", "schedule"});
constexpr auto kEnTimeMark = std::to_array<Phrase>({
    "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday", "tomorrow", "today", "tonight",
    "at", "next", "oclock"});
constexpr auto kEnCancelVerb = std::to_array<Phrase>({"cancel", "delete", "remove", "drop", "scrap", "take off"});
constexpr auto kEnQueryMarker = std::to_array<Phrase>({
    "what", "whats", "which", "tell me", "read me", "show me", "list", "when", "do i have", "how many", "give me",
    "read", "show"});
constexpr auto kEnFreeMarker = std::to_array<Phrase>({"am i free", "am i busy", "do i have time"});
constexpr auto kEnHaveScheduled = std::to_array<Phrase>({"anything scheduled", "anything planned", "have scheduled",
                                                          "have planned", "got scheduled", "any meetings", "any appointments",
                                                          "any events"});
constexpr auto kEnTaskNoun = std::to_array<Phrase>({"task", "tasks", "to do", "to dos", "chores"});
constexpr auto kEnNewMark = std::to_array<Phrase>({"new task", "new project", "new to do"});
constexpr auto kEnCompleteVerb = std::to_array<Phrase>({"complete", "finish"});
constexpr auto kEnCompleteStrong = std::to_array<Phrase>({"check off", "check it off", "tick off", "cross off", "check this off"});
constexpr auto kEnMarkVerb = std::to_array<Phrase>({"mark"});
constexpr auto kEnDoneMark = std::to_array<Phrase>({"as done", "as complete", "as completed", "as finished", "done", "finished", "complete", "completed"});
constexpr auto kEnProjectNoun = std::to_array<Phrase>({"project", "projects"});
constexpr auto kEnProjectVerb = std::to_array<Phrase>({"create", "start", "open", "begin", "i want", "i need", "set up"});
constexpr auto kEnScreenWord = std::to_array<Phrase>({"section", "screen", "go to", "take me"});
constexpr auto kEnModuleNoun = std::to_array<Phrase>({"module", "modules"});
constexpr auto kEnModulePlural = std::to_array<Phrase>({"modules"});
constexpr auto kEnBookingObject = std::to_array<Phrase>({"table", "room", "hotel", "flight", "ticket", "tickets", "ride", "taxi",
                                                           "restaurant", "court", "seat", "seats"});
constexpr auto kEnProjectVeto = std::to_array<Phrase>({"bill", "law project"});
constexpr auto kEnEnableVerb = std::to_array<Phrase>({"turn on", "turn it on", "switch on", "switch it on", "enable",
                                                       "activate", "install", "start using"});
constexpr auto kEnDisableVerb = std::to_array<Phrase>({"turn off", "turn it off", "switch off", "switch it off",
                                                        "disable", "deactivate", "stop using"});
constexpr auto kEnPurgeVerb = std::to_array<Phrase>({"delete", "wipe", "erase", "remove", "get rid of", "clear"});
constexpr auto kEnDataNoun = std::to_array<Phrase>({"data", "information", "everything it stored", "history"});
constexpr auto kEnRequestVerb = std::to_array<Phrase>({"ask", "tell", "request"});
constexpr auto kEnOwnerNoun = std::to_array<Phrase>({"owner", "administrator", "admin"});
constexpr auto kEnExplainMarker = std::to_array<Phrase>({"explain", "what does", "tell me about", "what can i do",
                                                          "describe", "what do i get", "how does", "how do i use"});
constexpr auto kEnExplainGeneric = std::to_array<Phrase>({"what is", "what are"});
constexpr auto kEnReminderNoun = std::to_array<Phrase>({"reminder", "reminders", "remind me", "let me know", "notify me"});
constexpr auto kEnDecline = std::to_array<Phrase>({"no", "never mind", "nevermind", "forget it", "leave it", "not now", "no thanks", "dont"});
constexpr auto kEnFiller = std::to_array<Phrase>({"please", "can", "you", "could", "would", "hey", "ok", "okay", "argus",
                                                   "so", "well", "then", "um", "uh", "i", "want", "to", "need"});

constexpr std::array<Table, 2> kTables{{
    {.language = "es",
     .groups = {kEsCreateVerb, kEsStrongCreate, kEsLeadAgenda, kEsEventNoun, kEsCalendarNoun, kEsTimeMark,
                kEsCancelVerb, kEsQueryMarker, kEsFreeMarker, kEsHaveScheduled, kEsTaskNoun, kEsNewMark,
                kEsCompleteVerb, kEsCompleteStrong, kEsMarkVerb, kEsDoneMark, kEsProjectNoun, kEsProjectVerb,
                kEsScreenWord, kEsModuleNoun, kEsModulePlural, kEsProjectVeto, kEsBookingObject, kEsEnableVerb, kEsDisableVerb, kEsPurgeVerb, kEsDataNoun,
                kEsRequestVerb, kEsOwnerNoun, kEsExplainMarker, kEsExplainGeneric, kEsReminderNoun, kEsDecline, kEsFiller}},
    {.language = "en",
     .groups = {kEnCreateVerb, kEnStrongCreate, kNothing, kEnEventNoun, kEnCalendarNoun, kEnTimeMark, kEnCancelVerb,
                kEnQueryMarker, kEnFreeMarker, kEnHaveScheduled, kEnTaskNoun, kEnNewMark, kEnCompleteVerb,
                kEnCompleteStrong, kEnMarkVerb, kEnDoneMark, kEnProjectNoun, kEnProjectVerb, kEnScreenWord,
                kEnModuleNoun, kEnModulePlural, kEnProjectVeto, kEnBookingObject, kEnEnableVerb, kEnDisableVerb, kEnPurgeVerb, kEnDataNoun, kEnRequestVerb,
                kEnOwnerNoun, kEnExplainMarker, kEnExplainGeneric, kEnReminderNoun, kEnDecline, kEnFiller}},
}};
}

std::span<const Table> tables()
{
  return kTables;
}

}
