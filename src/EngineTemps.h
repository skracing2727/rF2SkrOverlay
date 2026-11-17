#pragma once

class Renderer;
struct TelemInfoV01;

// Water and oil temperature box. rF2 gives plugins the two temperatures and
// one overheating flag (set when the car is already overheating), but not the
// car's limits and no engine wear. So each temperature is coloured against a
// "normal" learned from this session's own driving at speed: white = normal,
// amber = well above normal, red = the game's overheating flag.
namespace EngineTemps
{
    void OnTelemetry(const TelemInfoV01& info);   // telemetry thread
    void Reset();                                 // new session
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
