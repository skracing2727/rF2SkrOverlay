#include "TyreStore.h"
#include "InternalsPlugin.hpp"
#include <Windows.h>
#include <cstring>
#include <cctype>
#include <cstdio>

namespace
{
    const int kMaxCars = 128;

    // F1 tyre colours. Hardcoded rather than exposed as fifteen INI keys:
    // these are a fixed published convention, not a taste setting, and a
    // "soft" that is not red would be actively misleading.
    struct Compound
    {
        char  letter;
        float r, g, b;
    };

    const Compound kSoft   = { 'S', 0.855f, 0.161f, 0.110f };  // #DA291C
    const Compound kMedium = { 'M', 1.000f, 0.820f, 0.180f };  // #FFD12E
    const Compound kHard   = { 'H', 0.941f, 0.941f, 0.925f };  // #F0F0EC
    const Compound kInter  = { 'I', 0.263f, 0.690f, 0.165f };  // #43B02A
    const Compound kWet    = { 'W', 0.000f, 0.404f, 0.678f };  // #0067AD
    const Compound kUnknown = { '?', 0.65f, 0.65f, 0.65f };

    struct Entry
    {
        long id = -1;
        long lastLap = -1000;
        bool prevLimiter = false;
        bool haveSample = false;
        Compound front = kUnknown;
        Compound rear = kUnknown;
    };

    CRITICAL_SECTION g_lock;
    bool g_lockReady = false;
    Entry g_cars[kMaxCars];
    int g_count = 0;

    void EnsureLock()
    {
        if (g_lockReady)
            return;
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }

    bool ContainsNoCase(const char* haystack, const char* needle)
    {
        if (!haystack || !needle || !needle[0])
            return false;
        for (const char* h = haystack; *h; ++h)
        {
            const char* a = h;
            const char* b = needle;
            while (*a && *b &&
                   tolower(static_cast<unsigned char>(*a)) ==
                   tolower(static_cast<unsigned char>(*b)))
            {
                ++a;
                ++b;
            }
            if (!*b)
                return true;
        }
        return false;
    }

    // Fold a mod's compound name onto the five F1 categories.
    //
    // ORDER MATTERS. Wet and intermediate are tested first because a name
    // like "Intermediate" must not be reached by a later, looser test, and
    // because some mods label a full wet "Rain" or "Monsoon". The dry names
    // are tested longest-first for the same reason: "supersoft",
    // "ultrasoft" and "hypersoft" all contain "soft" and all mean the red
    // tyre as far as a standings column is concerned -- F1 only ever shows
    // three dry compounds on a given weekend regardless of what rubber is
    // actually underneath, so flattening the family is the right answer, not
    // a loss of information.
    Compound Fold(const char* name)
    {
        if (!name || !name[0])
            return kUnknown;

        if (ContainsNoCase(name, "inter") || ContainsNoCase(name, "intermediate"))
            return kInter;
        if (ContainsNoCase(name, "wet") || ContainsNoCase(name, "rain") ||
            ContainsNoCase(name, "monsoon"))
            return kWet;

        if (ContainsNoCase(name, "medium") || ContainsNoCase(name, "med"))
            return kMedium;
        if (ContainsNoCase(name, "hard"))
            return kHard;
        if (ContainsNoCase(name, "soft"))
            return kSoft;

        // Pirelli's colour naming, used by a few mods in place of the
        // compound itself.
        if (ContainsNoCase(name, "red"))
            return kSoft;
        if (ContainsNoCase(name, "yellow"))
            return kMedium;
        if (ContainsNoCase(name, "white"))
            return kHard;
        if (ContainsNoCase(name, "green"))
            return kInter;
        if (ContainsNoCase(name, "blue"))
            return kWet;

        // Nothing matched. Show the first letter in neutral grey rather than
        // nothing at all -- an unrecognised mod compound still distinguishes
        // one car from another, which is most of the value of the column.
        Compound c = kUnknown;
        c.letter = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
        return c;
    }

    // Linear scan. The field is 10-15 cars in practice and this runs on a
    // hot callback, so a hash would cost more in setup than it saves.
    Entry* Find(long id)
    {
        for (int i = 0; i < g_count; ++i)
            if (g_cars[i].id == id)
                return &g_cars[i];
        return nullptr;
    }
}

void TyreStore::OnTelemetry(const TelemInfoV01& info)
{
    EnsureLock();

    EnterCriticalSection(&g_lock);

    Entry* e = Find(info.mID);
    if (!e && g_count < kMaxCars)
    {
        e = &g_cars[g_count++];
        e->id = info.mID;
        e->lastLap = -1000;
        e->prevLimiter = false;
        e->haveSample = false;
    }

    if (e)
    {
        const bool limiterNow = info.mSpeedLimiter != 0;
        // Leaving the pit lane, not entering it: the limiter going OFF is the
        // moment a new set is actually on the car and running.
        const bool leftPits = e->prevLimiter && !limiterNow;
        const bool newLap = (info.mLapNumber != e->lastLap);

        if (newLap || leftPits || !e->haveSample)
        {
            e->front = Fold(info.mFrontTireCompoundName);
            e->rear = Fold(info.mRearTireCompoundName);
            e->lastLap = info.mLapNumber;
            e->haveSample = true;
        }
        e->prevLimiter = limiterNow;
    }

    LeaveCriticalSection(&g_lock);
}


bool TyreStore::Get(long id, char* out, unsigned int outSize, float& r, float& g, float& b)
{
    if (!out || outSize < 4 || !g_lockReady)
        return false;

    bool found = false;
    EnterCriticalSection(&g_lock);
    const Entry* e = Find(id);
    if (e && e->haveSample)
    {
        if (e->front.letter == e->rear.letter)
        {
            sprintf_s(out, outSize, "%c", e->front.letter);
        }
        else
        {
            // Split strategy -- rare, but real on some mods, and silently
            // showing only the front would hide it.
            sprintf_s(out, outSize, "%c/%c", e->front.letter, e->rear.letter);
        }
        // The fronts carry the strategy story, so they set the colour.
        r = e->front.r;
        g = e->front.g;
        b = e->front.b;
        found = true;
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

bool TyreStore::GetRearColour(long id, float& r, float& g, float& b)
{
    if (!g_lockReady)
        return false;
    bool found = false;
    EnterCriticalSection(&g_lock);
    const Entry* e = Find(id);
    if (e && e->haveSample)
    {
        r = e->rear.r;
        g = e->rear.g;
        b = e->rear.b;
        found = true;
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

void TyreStore::Clear()
{
    if (!g_lockReady)
        return;
    EnterCriticalSection(&g_lock);
    g_count = 0;
    LeaveCriticalSection(&g_lock);
}
