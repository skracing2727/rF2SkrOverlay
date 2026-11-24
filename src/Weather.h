#pragma once
#include <string>

struct ScoringInfoV01;

// Weather forecasts for the track conditions box, two side by side to compare:
//
//  GAME: rF2's own forecast, read from the game's local web API
//        (http://localhost:5397/rest/sessions/weather, the same source
//        TinyPedal uses). Five points across the session (start, 20%, 40%,
//        60%, 80%), each with sky type, air temperature and rain chance.
//        Only talks to this PC.
//
//  REAL: an hourly forecast from Open-Meteo (api.open-meteo.com, free, no
//        key) for the track's real coordinates -- rain probability, rain
//        amount and temperature for the next hours. Only the track's
//        coordinates are sent. Coordinates come from a built-in list of
//        circuits, or [Conditions] Coords_<track name>=lat,lon in the INI.
//        Off by default; on with [Conditions] RealWeather=1.
//
// Fetching runs on its own thread; nothing here blocks a game callback or a
// frame.
namespace Weather
{
    void OnScoring(const ScoringInfoV01& info);   // notes track/session, starts the thread
    void Reset();
    void Shutdown();

    struct GamePoint { float minutesAhead; int sky; float tempC; float rainChance; };
    struct RealPoint { int hoursAhead; float rainChance; float rainMm; float tempC; };

    // Up to `max` upcoming points; returns how many. Status text says why
    // there are none ("no data", "track not in list", ...) -- a string
    // literal, so the caller can keep the pointer.
    int Game(GamePoint* out, int max, const char*& status);
    int Real(RealPoint* out, int max, const char*& status);

    const char* SkyName(int sky);
}
