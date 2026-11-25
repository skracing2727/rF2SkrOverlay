#pragma once
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <string>

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
        // Make sure the folder exists (review F278: some stores never
        // created it themselves). A no-op when it is already there.
        char dir[kTmpPathSize];
        strncpy_s(dir, path, _TRUNCATE);
        if (char* slash = strrchr(dir, '\\'))
        {
            *slash = '\0';
            CreateDirectoryA(dir, nullptr);
        }
        FILE* f = nullptr;
        if (fopen_s(&f, tmp, mode) != 0)
            return nullptr;
        return f;
    }

    // Closes the temp file and moves it over 'path'. Any write error (or
    // 'ok' false, for a short write the caller noticed itself) discards the
    // temp file instead and leaves 'path' untouched.
    //
    // The data is flushed to disk before the rename and the rename itself is
    // write-through, so a power cut or blue screen right after a save cannot
    // leave a zero-filled file (review F278). The rename is retried a few
    // times when the target is briefly locked (an editor, antivirus). Save
    // thread only, so the waits cost no frame time. False = not saved; the
    // caller should keep the data for the next save pass.
    inline bool Commit(FILE* f, const char* tmp, const char* path, bool ok = true)
    {
        if (fflush(f) != 0 || ferror(f))
            ok = false;
        if (ok)
        {
            const intptr_t h = _get_osfhandle(_fileno(f));
            if (h != -1)
                FlushFileBuffers(reinterpret_cast<HANDLE>(h));
        }
        if (fclose(f) != 0)
            ok = false;
        if (ok)
        {
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return true;
                const DWORD err = GetLastError();
                if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION)
                    break;
                Sleep(50);
            }
        }
        DeleteFileA(tmp);
        return false;
    }

    // File-name form of a car/track name: letters, digits and '-' kept,
    // anything else becomes '_'. maxLen bounds the scan to the SDK's fixed
    // char[64] buffers (no guaranteed '\0' when a name fills one).
    //
    // review F420: names with non-ASCII or control bytes (Japanese, accented
    // letters, ...) used to collapse to the same "____" form and share one
    // file. Those now get "_" + 6 hex digits of an FNV-1a hash of the
    // original bytes appended, so they stay apart. Plain-ASCII names (where
    // only spaces/punctuation become '_') are unchanged, so every existing
    // file keeps its name.
    inline std::string SafeName(const char* s, size_t maxLen)
    {
        std::string out;
        bool lossy = false;
        unsigned int hash = 2166136261u;
        for (size_t i = 0; i < maxLen && s && s[i]; ++i)
        {
            const char c = s[i];
            const unsigned char u = static_cast<unsigned char>(c);
            hash = (hash ^ u) * 16777619u;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            if (u >= 0x80 || u < 0x20) lossy = true;
            out.push_back(ok ? c : '_');
        }
        if (out.empty()) return "unknown";
        if (lossy)
        {
            char tag[8];
            _snprintf_s(tag, sizeof(tag), _TRUNCATE, "_%06x", hash & 0xFFFFFFu);
            out += tag;
        }
        return out;
    }

    inline bool Exists(const char* path)
    {
        return path && path[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
}
