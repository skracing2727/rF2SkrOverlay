#pragma once

// The plugin's only disk-write points (see the disk rule in Persist.cpp).
//
// One background thread runs the save pass. Requests from any thread only set
// a flag and wake it, so nothing that draws a frame or handles a game
// callback ever waits for the disk. A burst of requests becomes one pass.
namespace Persist
{
    // Plugin Startup. 'pass' does the actual saving (Plugin.cpp): with
    // all == false only the learned driving data, with all == true everything.
    void Start(void (*pass)(bool all), void (*loadPass)() = nullptr);
    void Stop();            // Plugin Shutdown, after FlushAllNow
    void RequestLearned();  // any thread, cheap (sets a flag + event): the player just crossed the line,
                            // or something was just learned at the line
    void RequestAll();      // any thread, cheap: out of the car
    void RequestLoad();     // any thread, cheap: only read a file a store asked for, write nothing
                            // (runs loadPass; review F275)
    void FlushAllNow();     // blocks until a full pass has run (EndSession / Shutdown)
}

// Who is driving the player's car (review F30, F167, F252, F253). Defined and
// written only by Plugin.cpp's scoring callback (~5 Hz); read from any thread
// with memory_order_relaxed.
//
// g_localPlayerDriving: true only while rF2 marks a car as ours (mIsPlayer)
//   AND that car is under local human control (mControl == 0). False while
//   the AI has the car (Esc mid-race, after the flag), while riding along a
//   team-mate (the PlayerView fallback in Plugin.cpp), and with no car at all.
// g_localPlayerLapClean: true when g_localPlayerDriving held on every scoring
//   tick of the lap in progress. Re-armed at each line crossing AFTER the
//   scoring learners have run, so on the crossing tick it still describes the
//   lap that has just been completed.
//
// How to use: any store that LEARNS (best lap / PB trace, braking points,
// shift points, fuel or tyre rates, track map recordings) must skip learning
// when the flag is false: whole-lap data checked at the line against
// g_localPlayerLapClean, per-sample data (telemetry) against
// g_localPlayerDriving. Display code must NOT use them: the HUD still shows
// the car while riding along or while the AI drives.
#include <atomic>
extern std::atomic<bool> g_localPlayerDriving;
extern std::atomic<bool> g_localPlayerLapClean;
