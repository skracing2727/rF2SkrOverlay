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
    void Start(void (*pass)(bool all));
    void Stop();            // Plugin Shutdown, after FlushAllNow
    void RequestLearned();  // any thread, cheap (sets a flag + event): the player just crossed the line,
                            // or something was just learned at the line
    void RequestAll();      // any thread, cheap: out of the car
    void FlushAllNow();     // blocks until a full pass has run (EndSession / Shutdown)
}
