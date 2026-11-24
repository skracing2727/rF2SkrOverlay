#pragma once

// The game's own replay recording, for the frame-time stats box: whether
// replays are recorded, to disk or to RAM, the fidelity setting, and -- when
// recording to disk -- how big the recording file has grown.
//
// Where it comes from (nothing here is in the plugin API):
//  - UserData\player\player.JSON: "Record Replays", "Record To Memory" and
//    "Replay Base Fidelity" (1..4 = Low, Medium, High, Max).
//    Re-read only when the file's last-write time changes.
//  - Recording to disk, rF2 streams the session into UserData\Replays\
//    _vcr<number>.tmp and only writes the compressed .Vcr when the session
//    ends. The newest such file created since the game started is the live
//    one (older ones are left behind by sessions that never got saved); its
//    size is read from the file system without opening it for reading.
//  - Recording to RAM: the game does not say how much memory the replay
//    takes, and it cannot be told apart from the rest of rF2's memory, so
//    only "RAM" is shown (rF2 RAM in the same box covers it).
//
// Poll() runs on the stats poller thread (PerfStats::StartPoller); the
// render thread only copies the result. Reads only -- nothing is written.
namespace ReplayInfo
{
    void Poll();

    struct State
    {
        bool known = false;        // player.JSON was read
        bool recording = false;    // "Record Replays"
        bool toMemory = false;     // "Record To Memory"
        int fidelity = -1;         // "Replay Base Fidelity", -1 = not found
        bool haveFile = false;     // a live recording file was found
        double fileMB = 0.0;
    };
    State Get();

    // The options screen's name: Low / Medium / High / Max for 1..4;
    // nullptr otherwise (shown as the number).
    const char* FidelityName(int fidelity);
}
