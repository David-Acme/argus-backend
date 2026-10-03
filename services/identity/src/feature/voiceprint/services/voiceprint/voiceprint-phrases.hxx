#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <voice/voice-lang.hxx>

namespace voiceprint_phrases
{

inline constexpr std::array<std::string_view, 12>
    kSpanish{"Las hojas secas bailan con el viento de otoño.",
             "Un barco pequeño navega despacio hacia el puerto.",
             "Me gusta el pan caliente con un poco de miel.",
             "El reloj de la plaza marca las siete y cuarto.",
             "Mañana iremos a caminar junto al río.",
             "La guitarra suena mejor cuando está afinada.",
             "Tres gatos grises duermen sobre el sofá.",
             "El tren de la mañana llega puntual a la estación.",
             "Prefiero el café sin azúcar y bien cargado.",
             "Las montañas lejanas se cubren de nieve en invierno.",
             "Hoy el jardín huele a jazmín y a tierra mojada.",
             "Mi vecino toca el piano todas las tardes."};

inline constexpr std::array<std::string_view, 12>
    kEnglish{"Dry leaves are dancing in the autumn wind.",
             "A small boat drifts slowly toward the harbor.",
             "Warm bread with a little honey is my favorite.",
             "The clock in the square strikes a quarter past seven.",
             "Tomorrow we will walk along the quiet river.",
             "The old guitar sounds better when it is tuned.",
             "Three gray cats are sleeping on the sofa.",
             "The morning train arrives on time at the station.",
             "I like my coffee strong and without sugar.",
             "The distant mountains turn white in the winter.",
             "Today the garden smells of jasmine and rain.",
             "My neighbor plays the piano every afternoon."};

[[nodiscard]] std::vector<std::string> pick(VoiceLang lang, size_t count);

}
