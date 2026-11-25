#include "DriverAbbrev.h"
#include "InternalsPlugin.hpp"
#include <cstring>
#include <cstdio>

namespace
{
    const int kMaxCars = 128;
    const int kNameLen = 32;   // matches VehicleScoringInfoV01::mDriverName

    struct Entry
    {
        long id = -1;
        char name[kNameLen] = {};
        char code[4] = {};
        int place = 0;
        int colorIndex = -1;
    };

    // Hand-picked rather than evenly-spaced HSV, which reliably produces a
    // few muddy olives and browns that are hard to tell apart at dot size.
    // All are bright enough to read against the map's dark backing plate.
    // Sixteen is comfortably more than the 10-15 car fields this is for, so
    // nothing repeats in practice.
    struct Rgb { float r, g, b; };
    const Rgb kPalette[] = {
        { 0.90f, 0.10f, 0.29f },  // red
        { 0.24f, 0.71f, 0.29f },  // green
        { 1.00f, 0.88f, 0.10f },  // yellow
        { 0.26f, 0.39f, 0.85f },  // blue
        { 0.96f, 0.51f, 0.19f },  // orange
        { 0.57f, 0.12f, 0.71f },  // purple
        { 0.26f, 0.83f, 0.96f },  // cyan
        { 0.94f, 0.20f, 0.90f },  // magenta
        { 0.75f, 0.94f, 0.27f },  // lime
        { 0.98f, 0.75f, 0.83f },  // pink
        { 0.27f, 0.60f, 0.56f },  // teal
        { 0.86f, 0.75f, 1.00f },  // lavender
        { 1.00f, 0.55f, 0.41f },  // salmon
        { 0.13f, 0.70f, 0.67f },  // sea green
        { 1.00f, 0.70f, 0.28f },  // amber
        { 0.53f, 0.81f, 0.92f },  // sky
    };
    const int kPaletteSize = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));

    CRITICAL_SECTION g_lock;
    bool g_ready = false;
    Entry g_entries[kMaxCars];
    int g_count = 0;

    void EnsureLock()
    {
        // Unlike VehicleKey (which is initialised from Startup), this module
        // is only ever touched from the scoring thread and the render thread,
        // and the FIRST touch is always OnScoring -- the render side cannot
        // ask about a car before scoring has reported one. So a lazy init
        // here is safe: it runs on the scoring thread, before any reader can
        // have anything to read.
        if (g_ready)
            return;
        InitializeCriticalSection(&g_lock);
        g_ready = true;
    }

    // --- ASCII folding -----------------------------------------------------

    // Latin-1 supplement + the handful of Latin Extended-A codepoints that
    // actually turn up in driver names. Returns a short ASCII expansion, or
    // nullptr when the codepoint has no sensible letter form (punctuation,
    // symbols) and should simply be dropped.
    const char* FoldCodepoint(unsigned int cp)
    {
        if (cp < 0x80)
            return nullptr;   // caller handles plain ASCII

        switch (cp)
        {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
        case 0x100: case 0x101: case 0x102: case 0x103: case 0x104: case 0x105:
            return "A";
        case 0xC6: case 0xE6: return "AE";
        case 0xC7: case 0xE7:
        case 0x106: case 0x107: case 0x10C: case 0x10D:
            return "C";
        case 0x10E: case 0x10F: case 0xD0: case 0xF0: case 0x110: case 0x111:
            return "D";
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0x112: case 0x113: case 0x118: case 0x119: case 0x11A: case 0x11B:
            return "E";
        case 0x11E: case 0x11F: return "G";
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0x12A: case 0x12B: case 0x130: case 0x131:
            return "I";
        case 0x141: case 0x142: return "L";
        case 0xD1: case 0xF1: case 0x143: case 0x144: case 0x147: case 0x148:
            return "N";
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8:
        case 0x14C: case 0x14D: case 0x150: case 0x151:
            return "O";
        case 0x152: case 0x153: return "OE";
        case 0x154: case 0x155: case 0x158: case 0x159: return "R";
        case 0x15A: case 0x15B: case 0x160: case 0x161: case 0x15E: case 0x15F:
            return "S";
        case 0xDF: return "SS";
        case 0x162: case 0x163: case 0x164: case 0x165: return "T";
        case 0xDE: case 0xFE: return "TH";
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC:
        case 0x16A: case 0x16B: case 0x16E: case 0x16F: case 0x170: case 0x171:
            return "U";
        case 0xDD: case 0xFD: case 0xFF: case 0x178: return "Y";
        case 0x179: case 0x17A: case 0x17B: case 0x17C: case 0x17D: case 0x17E:
            return "Z";
        default: return nullptr;
        }
    }

    // Windows-1252 occupies 0x80..0x9F, where Latin-1 has control codes. Only
    // the letters matter here; everything else in that range folds away.
    unsigned int Cp1252ToCodepoint(unsigned char b)
    {
        switch (b)
        {
        case 0x8A: return 0x160;  // S with caron
        case 0x8C: return 0x152;  // OE
        case 0x8E: return 0x17D;  // Z with caron
        case 0x9A: return 0x161;
        case 0x9C: return 0x153;
        case 0x9E: return 0x17E;
        case 0x9F: return 0x178;  // Y with diaeresis
        default: return b;        // 0xA0..0xFF are Latin-1 already
        }
    }

    // Folds one character starting at 'p' into uppercase ASCII, appending to
    // 'out'. Returns how many BYTES were consumed.
    //
    // rFactor 2's encoding for mDriverName is not documented and has not been
    // confirmed on this install, so both are accepted: a well-formed UTF-8
    // sequence is decoded as such, and anything else is read as a single
    // Windows-1252 byte. A name that is pure ASCII -- the overwhelming
    // majority -- takes the same path either way.
    // Lower-case letter in Latin-1 / Latin Extended-A? (FoldToAscii keeps case.)
    bool IsLowerCodepoint(unsigned int cp)
    {
        if (cp >= 0xDF && cp <= 0xFF) return cp != 0xF7;
        if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) return (cp & 1u) == 0;
        if (cp >= 0x100 && cp <= 0x177) return (cp & 1u) == 1;
        return false;
    }

    int FoldOne(const unsigned char* p, size_t avail, char* out, int outCap, int& outLen, bool upper = true)
    {
        outLen = 0;
        const unsigned char b0 = p[0];

        if (b0 < 0x80)
        {
            if (outCap > 0)
            {
                char c = static_cast<char>(b0);
                if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                out[0] = c;
                outLen = 1;
            }
            return 1;
        }

        unsigned int cp = 0;
        int consumed = 1;
        // Valid UTF-8 lead byte with at least one continuation byte following?
        if (b0 >= 0xC2 && b0 <= 0xDF && avail >= 2 && (p[1] & 0xC0) == 0x80)
        {
            cp = ((b0 & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            consumed = 2;
        }
        else if (b0 >= 0xE0 && b0 <= 0xEF && avail >= 3 &&
                 (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
        {
            cp = ((b0 & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            consumed = 3;
        }
        else if (b0 >= 0xF0 && b0 <= 0xF4 && avail >= 4 && (p[1] & 0xC0) == 0x80
                 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
        {
            // 4-byte UTF-8 (emoji): no letter form, one separator (review F416).
            cp = 0x10000;
            consumed = 4;
        }
        else if (b0 >= 0xE0 && b0 <= 0xF4 && avail >= 2 && (p[1] & 0xC0) == 0x80)
        {
            // A multi-byte lead cut short (the name field is only 32 bytes):
            // swallow its continuation bytes rather than reading each as a
            // cp1252 letter, which turned emoji into fake letters (F416).
            consumed = 1;
            while (consumed < 4 && static_cast<size_t>(consumed) < avail && (p[consumed] & 0xC0) == 0x80)
                ++consumed;
            cp = 0x10000;
        }
        else
        {
            cp = Cp1252ToCodepoint(b0);
            consumed = 1;
        }

        const char* fold = FoldCodepoint(cp);
        if (fold)
        {
            const bool lower = !upper && IsLowerCodepoint(cp);
            int n = 0;
            while (fold[n] && n < outCap)
            {
                char c = fold[n];
                if (lower && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                out[n++] = c;
            }
            outLen = n;
        }
        return consumed;
    }

    // Whole name -> uppercase ASCII, non-letters collapsed to single spaces so
    // handles like "paul_kiernan_27" tokenise the same way "Paul Kiernan"
    // does.
    void NormalizeName(const char* src, char* out, size_t outSize)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
        const size_t srcLen = strnlen(src, kNameLen);
        size_t w = 0;
        bool lastWasSpace = true;   // true so leading separators are dropped

        size_t i = 0;
        while (i < srcLen && w + 1 < outSize)
        {
            char folded[4];
            int foldedLen = 0;
            i += static_cast<size_t>(FoldOne(p + i, srcLen - i, folded, 4, foldedLen));

            for (int k = 0; k < foldedLen && w + 1 < outSize; ++k)
            {
                const char c = folded[k];
                if (c >= 'A' && c <= 'Z')
                {
                    out[w++] = c;
                    lastWasSpace = false;
                }
                else if (!lastWasSpace)
                {
                    out[w++] = ' ';
                    lastWasSpace = true;
                }
            }
            // A codepoint with no letter form (punctuation, an emoji in a
            // multiplayer handle) still separates tokens.
            if (foldedLen == 0 && !lastWasSpace && w + 1 < outSize)
            {
                out[w++] = ' ';
                lastWasSpace = true;
            }
        }
        while (w > 0 && out[w - 1] == ' ')
            --w;
        out[w] = 0;
    }

    bool IsGenerationalSuffix(const char* tok, int len)
    {
        struct { const char* s; int n; } kSuffixes[] = {
            { "JR", 2 }, { "JNR", 3 }, { "SR", 2 }, { "SNR", 3 },
            { "II", 2 }, { "III", 3 }, { "IV", 2 },
        };
        for (const auto& s : kSuffixes)
            if (len == s.n && strncmp(tok, s.s, static_cast<size_t>(len)) == 0)
                return true;
        return false;
    }

    struct Token { const char* p; int len; };

    int Tokenize(const char* norm, Token* out, int maxTokens)
    {
        int n = 0;
        const char* p = norm;
        while (*p && n < maxTokens)
        {
            while (*p == ' ') ++p;
            if (!*p) break;
            const char* start = p;
            while (*p && *p != ' ') ++p;
            out[n].p = start;
            out[n].len = static_cast<int>(p - start);
            ++n;
        }
        return n;
    }

    // Builds the preferred 3-letter code, ignoring what anyone else has.
    void BaseCode(const char* rawName, char* out /*[4]*/, char* outGivenInitial,
                  Token& outSurname, char* normBuf, size_t normSize)
    {
        NormalizeName(rawName, normBuf, normSize);

        Token toks[12];
        int nTok = Tokenize(normBuf, toks, 12);

        outGivenInitial[0] = 0;
        outSurname.p = nullptr;
        outSurname.len = 0;

        if (nTok == 0)
        {
            // Nothing usable at all (empty, or a name of pure punctuation).
            // The caller's collision pass will make this unique.
            strcpy_s(out, 4, "???");
            return;
        }

        // Last token, skipping generational suffixes.
        int surnameIdx = nTok - 1;
        while (surnameIdx > 0 && IsGenerationalSuffix(toks[surnameIdx].p, toks[surnameIdx].len))
            --surnameIdx;

        // A last token too short to abbreviate ("Xx" in a handle like
        // "xX_Racer_Xx", or a two-letter surname) yields a better code from
        // the longest token instead -- which for a real name is still the
        // surname often enough, and for a handle picks the memorable word.
        if (toks[surnameIdx].len < 3)
        {
            int longest = surnameIdx;
            for (int i = 0; i < nTok; ++i)
                if (toks[i].len > toks[longest].len)
                    longest = i;
            if (toks[longest].len > toks[surnameIdx].len)
                surnameIdx = longest;
        }

        outSurname = toks[surnameIdx];
        if (surnameIdx != 0)
            outGivenInitial[0] = toks[0].p[0];
        outGivenInitial[1] = 0;

        int w = 0;
        for (int i = 0; i < outSurname.len && w < 3; ++i)
            out[w++] = outSurname.p[i];
        // Short surname: top up from the given name, then pad, so the code is
        // always exactly three characters wide (the map's label column
        // assumes it).
        for (int t = 0; t < nTok && w < 3; ++t)
        {
            if (t == surnameIdx) continue;
            for (int i = 0; i < toks[t].len && w < 3; ++i)
                out[w++] = toks[t].p[i];
        }
        while (w < 3)
            out[w++] = 'X';
        out[3] = 0;
    }

    bool CodeTaken(const char (*codes)[4], int count, const char* candidate)
    {
        for (int i = 0; i < count; ++i)
            if (codes[i][0] && strcmp(codes[i], candidate) == 0)
                return true;
        return false;
    }
}

void DriverAbbrev::OnScoring(const ScoringInfoV01& info)
{
    EnsureLock();

    const int n = (info.mNumVehicles < kMaxCars) ? info.mNumVehicles : kMaxCars;
    if (n <= 0 || !info.mVehicle)
    {
        EnterCriticalSection(&g_lock);
        g_count = 0;
        LeaveCriticalSection(&g_lock);
        return;
    }

    // Cheap path: if the same cars with the same names are present, only the
    // places can have moved, so refresh those and skip the whole abbreviation
    // pass. Names never change mid-session in practice, so this is the case
    // that runs ~5 times a second for the life of a race.
    bool sameField = false;
    EnterCriticalSection(&g_lock);
    if (g_count == n)
    {
        sameField = true;
        for (int i = 0; i < n && sameField; ++i)
        {
            // Scoring reorders mVehicle[] by running order, so match on ID
            // rather than assuming index i is still the same car.
            const long id = info.mVehicle[i].mID;
            bool found = false;
            for (int j = 0; j < g_count; ++j)
            {
                if (g_entries[j].id != id) continue;
                found = strncmp(g_entries[j].name, info.mVehicle[i].mDriverName, kNameLen) == 0;
                break;
            }
            sameField = found;
        }
    }
    if (sameField)
    {
        for (int i = 0; i < n; ++i)
        {
            const long id = info.mVehicle[i].mID;
            for (int j = 0; j < g_count; ++j)
                if (g_entries[j].id == id)
                {
                    g_entries[j].place = info.mVehicle[i].mPlace;
                    break;
                }
        }
    }
    LeaveCriticalSection(&g_lock);
    if (sameField)
        return;

    // --- Full re-abbreviation of the field --------------------------------

    // The table as it stands, so an existing car keeps the colour it already
    // has. Without this, one car joining or retiring would recolour the whole
    // field mid-session.
    Entry prevEntries[kMaxCars];
    int prevCount = 0;
    EnterCriticalSection(&g_lock);
    prevCount = g_count;
    for (int i = 0; i < prevCount; ++i)
        prevEntries[i] = g_entries[i];
    LeaveCriticalSection(&g_lock);

    // Assignment order is ascending mID, NOT scoring array order: mID is
    // stable for the life of the session while the array is re-sorted by
    // running order every tick. Sorting here is what stops two colliding
    // drivers from trading codes whenever they trade places.
    int order[kMaxCars];
    for (int i = 0; i < n; ++i)
        order[i] = i;
    for (int i = 1; i < n; ++i)
    {
        const int key = order[i];
        int j = i - 1;
        while (j >= 0 && info.mVehicle[order[j]].mID > info.mVehicle[key].mID)
        {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }

    char codes[kMaxCars][4] = {};
    Entry built[kMaxCars];

    for (int slot = 0; slot < n; ++slot)
    {
        const VehicleScoringInfoV01& v = info.mVehicle[order[slot]];

        char norm[128];
        char base[4];
        char givenInitial[2];
        Token surname;
        BaseCode(v.mDriverName, base, givenInitial, surname, norm, sizeof(norm));

        char chosen[4];
        strcpy_s(chosen, sizeof(chosen), base);

        if (CodeTaken(codes, slot, chosen))
        {
            bool resolved = false;

            // 1. Given-name initial + two of the surname: Michael and Ralf
            //    Schumacher become MSC and RSC, the same convention real
            //    timing screens use.
            if (givenInitial[0] && surname.len >= 2)
            {
                char cand[4] = { givenInitial[0], surname.p[0], surname.p[1], 0 };
                if (!CodeTaken(codes, slot, cand))
                {
                    strcpy_s(chosen, sizeof(chosen), cand);
                    resolved = true;
                }
            }

            // 2. Keep the first two letters and walk deeper into the surname
            //    for the third: SCHUMACHER -> SCU, SCM, SCA...
            for (int k = 3; !resolved && k < surname.len; ++k)
            {
                char cand[4] = { surname.p[0], surname.p[1], surname.p[k], 0 };
                if (!CodeTaken(codes, slot, cand))
                {
                    strcpy_s(chosen, sizeof(chosen), cand);
                    resolved = true;
                }
            }

            // 3. Guaranteed terminator: 26 letters plus 9 digits in the third
            //    slot cannot run out for any field this game can hold.
            for (int k = 0; !resolved && k < 35; ++k)
            {
                const char tail = (k < 26) ? static_cast<char>('A' + k)
                                           : static_cast<char>('1' + (k - 26));
                char cand[4] = { base[0], base[1], tail, 0 };
                if (!CodeTaken(codes, slot, cand))
                {
                    strcpy_s(chosen, sizeof(chosen), cand);
                    resolved = true;
                }
            }
        }

        strcpy_s(codes[slot], sizeof(codes[slot]), chosen);

        built[slot].id = v.mID;
        strncpy_s(built[slot].name, kNameLen, v.mDriverName, _TRUNCATE);
        strcpy_s(built[slot].code, sizeof(built[slot].code), chosen);
        built[slot].place = v.mPlace;
    }

    // Colours: carry over first, then fill the gaps from whatever is left
    // unused, so a new arrival takes a free colour rather than duplicating
    // one already on track.
    bool used[kPaletteSize] = {};
    for (int slot = 0; slot < n; ++slot)
    {
        built[slot].colorIndex = -1;
        for (int j = 0; j < prevCount; ++j)
        {
            if (prevEntries[j].id != built[slot].id)
                continue;
            const int ci = prevEntries[j].colorIndex;
            if (ci >= 0 && ci < kPaletteSize && !used[ci])
            {
                built[slot].colorIndex = ci;
                used[ci] = true;
            }
            break;
        }
    }
    for (int slot = 0; slot < n; ++slot)
    {
        if (built[slot].colorIndex >= 0)
            continue;
        int pick = -1;
        for (int c = 0; c < kPaletteSize && pick < 0; ++c)
            if (!used[c])
                pick = c;
        // More cars than colours: wrap rather than leave anyone uncoloured.
        // Only reachable above 16 entries, which these grids do not reach.
        if (pick < 0)
            pick = slot % kPaletteSize;
        else
            used[pick] = true;
        built[slot].colorIndex = pick;
    }

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < n; ++i)
        g_entries[i] = built[i];
    g_count = n;
    LeaveCriticalSection(&g_lock);
}

bool DriverAbbrev::Color(long id, float& r, float& g, float& b)
{
    if (!g_ready)
        return false;
    bool found = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_count; ++i)
    {
        if (g_entries[i].id != id)
            continue;
        const int ci = g_entries[i].colorIndex;
        if (ci >= 0 && ci < kPaletteSize)
        {
            r = kPalette[ci].r;
            g = kPalette[ci].g;
            b = kPalette[ci].b;
            found = true;
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

bool DriverAbbrev::Get(long id, char* out, size_t outSize)
{
    if (!out || outSize == 0)
        return false;
    out[0] = 0;
    if (!g_ready)
        return false;

    bool found = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_count; ++i)
    {
        if (g_entries[i].id == id)
        {
            strncpy_s(out, outSize, g_entries[i].code, _TRUNCATE);
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

int DriverAbbrev::Place(long id)
{
    if (!g_ready)
        return 0;
    int place = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_count; ++i)
    {
        if (g_entries[i].id == id)
        {
            place = g_entries[i].place;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return place;
}

bool DriverAbbrev::NameParts(long id, char* first, size_t firstSize,
                             char* last, size_t lastSize)
{
    if (!first || !last || firstSize == 0 || lastSize == 0)
        return false;
    first[0] = 0;
    last[0] = 0;
    if (!g_ready)   // the lock is made by the first OnScoring
        return false;

    char raw[kNameLen] = {};
    bool found = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_count; ++i)
    {
        if (g_entries[i].id == id)
        {
            strncpy_s(raw, kNameLen, g_entries[i].name, _TRUNCATE);
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (!found)
        return false;

    char norm[kNameLen * 2] = {};
    NormalizeName(raw, norm, sizeof(norm));

    Token toks[8];
    const int n = Tokenize(norm, toks, 8);
    if (n <= 0)
        return false;

    // Surname is the last token that is not a generational suffix -- the
    // same rule the code's base uses, so the two never disagree about which
    // part of the name is the surname.
    int lastIdx = n - 1;
    while (lastIdx > 0 && IsGenerationalSuffix(toks[lastIdx].p, toks[lastIdx].len))
        --lastIdx;

    const int lastLen = toks[lastIdx].len;
    const size_t lastCopy = (static_cast<size_t>(lastLen) < lastSize - 1)
                          ? static_cast<size_t>(lastLen) : lastSize - 1;
    memcpy(last, toks[lastIdx].p, lastCopy);
    last[lastCopy] = 0;

    // Everything before the surname, space-joined. Usually one token; a
    // double-barrelled given name keeps both rather than being truncated.
    size_t w = 0;
    for (int i = 0; i < lastIdx; ++i)
    {
        if (w && w + 1 < firstSize)
            first[w++] = ' ';
        for (int k = 0; k < toks[i].len && w + 1 < firstSize; ++k)
            first[w++] = toks[i].p[k];
    }
    first[w] = 0;
    return true;
}

int DriverAbbrev::Snapshot(CarInfo* out, int maxCars)
{
    if (!out || maxCars <= 0 || !g_ready)
        return 0;
    EnterCriticalSection(&g_lock);
    const int n = (g_count < maxCars) ? g_count : maxCars;
    for (int i = 0; i < n; ++i)
    {
        const Entry& e = g_entries[i];
        CarInfo& c = out[i];
        c.id = e.id;
        c.place = e.place;
        const int ci = e.colorIndex;
        c.hasColor = ci >= 0 && ci < kPaletteSize;
        c.r = c.hasColor ? kPalette[ci].r : 0.0f;
        c.g = c.hasColor ? kPalette[ci].g : 0.0f;
        c.b = c.hasColor ? kPalette[ci].b : 0.0f;
        memcpy(c.code, e.code, sizeof(c.code));
        c.code[sizeof(c.code) - 1] = 0;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

void DriverAbbrev::Clear()
{
    if (!g_ready)
        return;
    EnterCriticalSection(&g_lock);
    g_count = 0;
    LeaveCriticalSection(&g_lock);
}

void DriverAbbrev::FoldToAscii(const char* src, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!src) return;
    // Case kept, accents folded, symbols/emoji dropped (review F89): the
    // font atlas only has ASCII 32..126 and skips anything else.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
    const size_t srcLen = strnlen(src, kNameLen);
    size_t w = 0;
    size_t i = 0;
    while (i < srcLen && w + 1 < outSize)
    {
        char folded[4];
        int foldedLen = 0;
        i += static_cast<size_t>(FoldOne(p + i, srcLen - i, folded, 4, foldedLen, false));
        for (int k = 0; k < foldedLen && w + 1 < outSize; ++k)
        {
            const char c = folded[k];
            if (c < 32 || c > 126) continue;
            if (c == ' ' && (w == 0 || out[w - 1] == ' ')) continue;
            out[w++] = c;
        }
    }
    while (w > 0 && out[w - 1] == ' ')
        --w;
    out[w] = 0;
}
