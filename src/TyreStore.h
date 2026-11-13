#pragma once

// Global scope: see the note in DriverAbbrev.h.
struct TelemInfoV01;

// Per-car tyre compound, folded to an F1-style letter and colour.
//
// WHY THIS IS ITS OWN STORE: compound lives in TelemInfoV01, not in scoring,
// so it arrives on a different callback at a different rate from everything
// else the timing table shows, and has to survive between scoring ticks.
// Keyed by car ID for that reason.
//
// SAMPLED ONCE PER LAP, not continuously. With all-vehicle telemetry the
// callback arrives ~3000 times a second across the field, and a compound
// only ever changes at a pit stop -- so the per-call cost is an ID lookup and
// a lap compare, and the string work happens on a lap change or on leaving
// the pit lane (speed limiter released), whichever comes first. The latter
// matters: waiting for the stripe would show the old compound for most of an
// out-lap, which is exactly the lap you want it on.
namespace TyreStore
{
    // Telemetry thread, every car, very often. Cheap unless something changed.
    void OnTelemetry(const TelemInfoV01& info);

    // Render thread. Writes a 1-2 character code ("S", "M", "S/M" when the
    // axles differ) and its colour. Returns false when this car has never
    // reported telemetry -- the caller shows a dash rather than a guess.
    bool Get(long id, char* out, unsigned int outSize, float& r, float& g, float& b);

    void Clear();
}
