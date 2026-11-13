#pragma once

// Minimal file logger. Writes next to the DLL so we can see exactly how far
// startup got when something goes wrong inside the game process.
namespace Log
{
    void Init();

    // Buffers the line in memory while actually driving (g_inRealtime) --
    // see FlushBuffered() -- and writes straight through otherwise, exactly
    // as before. Disk touches while driving were a real, measured source of
    // Plugin CPU spikes (fopen/write/fclose every call), and this is called
    // from many ordinary event sites during a session, not just rare ones.
    void Write(const char* fmt, ...);

    // Writes everything buffered by Write() above to disk in one open/write/
    // close, then clears the buffer. Called once back at the garage/menu
    // (see Plugin.cpp) -- or directly, bypassing the wait, for something
    // worth the disk-I/O risk immediately regardless (a caught exception:
    // losing that diagnostic to an unflushed buffer would defeat the point
    // of logging it at all).
    void FlushBuffered();
}
