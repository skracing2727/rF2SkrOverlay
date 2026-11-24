#pragma once

// The short synthesized cues (brake point, DRS, the slow-car warning) are
// played from one dedicated thread rather than from the code that decides to
// beep.
//
// PlaySound is not free even with SND_ASYNC: the very first call loads and
// opens the audio stack, and every call opens the wave device again. Called
// directly, that cost landed on the game's telemetry and scoring callbacks
// (once inside a lock the render thread also takes every frame). Now the caller only hands over a pointer and sets an event.
namespace Sound
{
    // Plays an in-memory WAV like PlaySound(SND_MEMORY|SND_ASYNC|SND_NODEFAULT), but from a dedicated
    // sound thread: never blocks the caller, latest request wins. Buffer must outlive the sound.
    void PlayMemory(const void* wav);

    // Optional, any thread, cheap after the first call: starts the sound
    // thread now and has it play a few ms of silence, so the audio stack's
    // one-off setup is done before the first real cue rather than delaying
    // it. Call it once the game's own audio is up (entering the car), not at
    // plugin Startup: an rF2 crash in its vehicle sound init once coincided
    // with audio being set up during Startup. PlayMemory starts the thread
    // itself if this was never called.
    void Prepare();

    void Shutdown();        // called from Plugin Shutdown
}
