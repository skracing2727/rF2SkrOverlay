#pragma once
#include <Windows.h>
#include <cstdio>

// Whole-file rewrites for the learned-data stores (best laps, brake profiles,
// fuel stats and profiles, tyre profiles, pit limits, shift points, track
// maps). The content goes to "<path>.tmp" first and only replaces the real
// file, in one rename, once all of it has been written -- so the game being
// killed, a crash or a full disk mid-save leaves the previous file as it was
// rather than a truncated one that would load as garbage next session.
//
// Only ever called from the save thread (see Persist.cpp), never while
// holding a lock that a game callback or the render thread takes.
namespace StoreFile
{
    // Room for any MAX_PATH path plus the ".tmp" suffix.
    const size_t kTmpPathSize = MAX_PATH + 8;

    // Opens the temp file for 'path' with fopen 'mode' ("w" text, "wb"
    // binary) and writes its name to 'tmp' for Commit(). nullptr on failure.
    inline FILE* Begin(const char* path, char* tmp, size_t tmpSize, const char* mode)
    {
        if (_snprintf_s(tmp, tmpSize, _TRUNCATE, "%s.tmp", path) < 0)
            return nullptr;
        FILE* f = nullptr;
        if (fopen_s(&f, tmp, mode) != 0)
            return nullptr;
        return f;
    }

    // Closes the temp file and moves it over 'path'. Any write error (or
    // 'ok' false, for a short write the caller noticed itself) discards the
    // temp file instead and leaves 'path' untouched.
    inline bool Commit(FILE* f, const char* tmp, const char* path, bool ok = true)
    {
        if (ferror(f))
            ok = false;
        if (fclose(f) != 0)
            ok = false;
        if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
            return true;
        DeleteFileA(tmp);
        return false;
    }
}
