#pragma once

// One HUD layout per car: which panels are on, and where and how big each
// one is. Kept in rF2SkrOverlayPublic_Layouts.ini beside the DLL, one section per
// car; the live values stay in the main INI as always, and are swapped when
// the car changes.
//
// WHICH CAR. The plugin API only gives a car's class and its entry name (the
// entry changes with livery and team). The car itself is read from rF2's
// player profile, UserData\<player>\<player>.JSON, "Vehicle File" -- the
// folder under Installed\Vehicles\ is the car model, shared by every livery.
// That file may lag the actual choice, so it is only trusted while the folder
// has always come with the same class; otherwise (or if it cannot be read)
// the key falls back to the class. The key used is logged.
//
// WHEN. The first time a car is seen it inherits the layout already on
// screen. From then on, changes made while in it belong to it alone: they
// are saved to its section out of the car (FlushIfDirty), and on switching
// to another car first.
struct ScoringInfoV01;

namespace CarLayout
{
    // Scoring thread: notes the player's car (no disk access here).
    void OnScoring(const ScoringInfoV01& info);

    // Render thread, once per frame after Shortcuts::Poll(): on a car change,
    // saves the old car's layout and applies the new one's.
    void Poll();

    // Out of the car only, with the plugin's other FlushIfDirty() calls:
    // saves the current car's layout if anything changed.
    void FlushIfDirty();
}
