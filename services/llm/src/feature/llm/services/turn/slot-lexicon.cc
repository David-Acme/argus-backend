#include "slot-lexicon.hxx"

#include <array>

namespace slot_lexicon
{

namespace
{
using Phrase = std::string_view;

constexpr auto kEsClitic = std::to_array<Phrase>({"me", "te", "nos", "lo", "la", "le", "se"});
constexpr auto kEsDeterminer = std::to_array<Phrase>({"un", "una", "unos", "unas", "el", "la", "los", "las", "mi", "mis",
                                                       "tu", "tus", "su", "sus", "este", "esta", "ese", "esa"});
constexpr auto kEsConnector = std::to_array<Phrase>({"para", "de", "que", "sobre", "por"});
constexpr auto kEsPreposition = std::to_array<Phrase>({"en", "a", "al", "del", "como"});
constexpr auto kEsTimeAnchor = std::to_array<Phrase>({
    "lunes", "martes", "miercoles", "jueves", "viernes", "sabado", "domingo", "manana", "hoy", "pasado manana",
    "a las", "a la", "esta tarde", "esta noche", "mediodia", "medianoche", "proxima semana", "fin de semana",
    "semana que viene"});
constexpr auto kEsTimeFiller = std::to_array<Phrase>({
    "el", "la", "las", "los", "a", "de", "del", "en", "por", "y", "e", "media", "cuarto", "punto", "tarde", "noche",
    "madrugada", "mediodia", "manana", "proximo", "proxima", "este", "esta", "que", "viene", "uno", "una", "dos",
    "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve", "diez", "once", "doce", "trece", "catorce", "quince",
    "veinte", "veintiuno", "veintidos", "veintitres", "veinticuatro", "veinticinco", "treinta", "cuarenta",
    "cincuenta", "dia", "semana", "mes", "hoy", "pasado", "am", "pm", "lunes", "martes", "miercoles", "jueves",
    "viernes", "sabado", "domingo", "todo", "para"});
constexpr auto kEsNaming = std::to_array<Phrase>({"llamado", "llamada", "titulado", "titulada", "denominado"});
constexpr auto kEsAnswerPrefix = std::to_array<Phrase>({"se llama", "llamalo", "llamala", "ponle", "que se llame",
                                                         "el titulo es", "el nombre es", "es", "sera", "pues"});
constexpr auto kEsModal = std::to_array<Phrase>({"puedes", "podrias", "podria", "quiero", "necesito", "quisiera",
                                                  "me gustaria", "por favor", "porfa", "ayudame a", "ayudame",
                                                  "eh", "em", "ok", "oye", "argus", "a ver", "ya", "pues", "bueno",
                                                  "mira", "entonces", "y", "ahorita", "ahora", "dale", "no", "mejor", "mas bien"});

constexpr auto kEsOther = std::to_array<Phrase>({"la otra", "el otro", "lo otro", "las otras", "los otros", "la otra opcion",
                                                  "la segunda", "el segundo", "la ultima", "el ultimo",
                                                  "mejor la otra", "mejor el otro"});

constexpr auto kEnClitic = std::to_array<Phrase>({"me", "us", "it"});
constexpr auto kEnDeterminer = std::to_array<Phrase>({"a", "an", "the", "my", "your", "some", "this", "that"});
constexpr auto kEnConnector = std::to_array<Phrase>({"to", "for", "about", "of"});
constexpr auto kEnPreposition = std::to_array<Phrase>({"on", "in", "into", "to", "onto", "as"});
constexpr auto kEnTimeAnchor = std::to_array<Phrase>({
    "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday", "tomorrow", "today", "tonight",
    "noon", "midnight", "next week", "this week", "this afternoon", "this evening", "this morning", "at"});
constexpr auto kEnTimeFiller = std::to_array<Phrase>({
    "on", "at", "in", "the", "of", "this", "next", "o", "clock", "oclock", "and", "half", "past", "quarter", "morning",
    "afternoon", "evening", "night", "day", "after", "am", "pm", "a", "m", "p", "one", "two", "three", "four", "five",
    "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "twenty", "thirty",
    "fortyfive", "forty", "fifty", "week", "month", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday",
    "sunday", "tomorrow", "today", "tonight", "noon", "fifteenth", "first", "second", "third", "for"});
constexpr auto kEnNaming = std::to_array<Phrase>({"called", "named", "titled", "entitled"});
constexpr auto kEnAnswerPrefix = std::to_array<Phrase>({"call it", "name it", "its called", "it is called", "the title is",
                                                         "the name is", "its", "it is"});
constexpr auto kEnModal = std::to_array<Phrase>({"can you", "could you", "would you", "please", "i want you to",
                                                  "i need you to", "i want", "i need", "hey", "ok", "okay", "argus",
                                                  "so", "well", "then", "um", "uh", "just", "no", "rather", "actually", "instead"});

constexpr auto kEnOther = std::to_array<Phrase>({"the other", "the other one", "the other option", "other one", "the second",
                                                  "the second one", "the latter", "the last one", "the other way"});

constexpr std::array<Table, 2> kTables{{
    {.language = "es",
     .groups = {kEsClitic, kEsDeterminer, kEsConnector, kEsPreposition, kEsTimeAnchor, kEsTimeFiller, kEsNaming,
                kEsAnswerPrefix, kEsModal, kEsOther}},
    {.language = "en",
     .groups = {kEnClitic, kEnDeterminer, kEnConnector, kEnPreposition, kEnTimeAnchor, kEnTimeFiller, kEnNaming,
                kEnAnswerPrefix, kEnModal, kEnOther}},
}};
}

std::span<const Table> tables()
{
  return kTables;
}

}
