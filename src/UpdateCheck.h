#pragma once

// Asks GitHub, once per game start, which release of this plugin is the
// latest, so the menus can say when a newer one is out.
//
//  - One HTTPS GET to api.github.com/repos/skracing2727/rF2SkrOverlay/
//    releases/latest on its own short-lived thread, started from Startup();
//    nothing on the render, telemetry or scoring threads, nothing written to
//    disk. The answer's "tag_name" ("v26.11.17.3") is compared with
//    RF2RO_VERSION.
//  - Nothing is sent but the request itself (GitHub sees the IP address, as
//    for any web page). Nothing is downloaded or installed.
//  - Any failure (offline, firewall, GitHub down, rate limit) is logged once
//    and otherwise silent.
//  - [Updates] CheckForUpdates=0 turns it off.
namespace UpdateCheck
{
    void Start();
    // Aborts a request still in flight and waits (briefly) for the thread,
    // so the DLL is never unloaded under it.
    void Shutdown();

    // True once GitHub answered with a release newer than this build;
    // 'latest' gets its version ("26.11.24.1"). Render thread: lock-free.
    bool NewerAvailable(char* latest, size_t latestSize);
}
