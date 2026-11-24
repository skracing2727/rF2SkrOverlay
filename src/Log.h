#pragma once

// Minimal file logger. Writes next to the DLL so we can see exactly how far
// startup got when something goes wrong inside the game process.
namespace Log
{
    void Init();

    // Buffers the line in memory and never touches the disk itself (the disk
    // rule, Persist.cpp): file writes from the render or game threads were a
    // real, measured source of Plugin CPU spikes (fopen/write/fclose every
    // call), and this is called from many ordinary event sites during a
    // session, not just rare ones. Out of the car it asks the Persist thread
    // for a pass, so the line still reaches the file within about a second;
    // while driving it waits until the car is left.
    void Write(const char* fmt, ...);

    // Writes everything buffered by Write() above to disk in one open/write/
    // close, then clears the buffer. Called by the Persist thread's "all"
    // pass -- or directly, bypassing the wait, for something worth the
    // disk-I/O risk immediately regardless (a caught exception in the render
    // hook: losing that diagnostic to an unflushed buffer would defeat the
    // point of logging it at all).
    void FlushBuffered();
}
