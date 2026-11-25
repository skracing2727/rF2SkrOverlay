#include "Weather.h"
#include "EcoThread.h"
#include "InternalsPlugin.hpp"
#include "Config.h"
#include "Log.h"
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#pragma comment(lib, "winhttp.lib")

namespace
{
    // Circuits by a lower-case piece of rF2's track name -> real coordinates.
    // First match wins, so more specific names come first. Add or override
    // any track in the INI: [Conditions] CoordsLat_<track>= / CoordsLon_<track>=
    // (track name with anything not a letter or digit as '_').
    struct Circuit { const char* key; double lat, lon; };
    const Circuit kCircuits[] = {
        { "spa", 50.4372, 5.9714 },        { "francorchamps", 50.4372, 5.9714 },
        { "sarthe", 47.9560, 0.2070 },     { "le mans", 47.9560, 0.2070 },   { "lemans", 47.9560, 0.2070 },
        { "nordschleife", 50.3356, 6.9475 }, { "nurburg", 50.3356, 6.9475 }, { "nuerburg", 50.3356, 6.9475 },
        { "daytona", 29.1852, -81.0705 },  { "sebring", 27.4547, -81.3483 },
        { "monza", 45.6156, 9.2811 },      { "imola", 44.3439, 11.7167 },     { "mugello", 43.9975, 11.3719 },
        { "silverstone", 52.0786, -1.0169 }, { "brands", 51.3569, 0.2628 },   { "donington", 52.8306, -1.3750 },
        { "oulton", 53.1800, -2.6136 },    { "snetterton", 52.4636, 0.9456 },
        { "bathurst", -33.4483, 149.5547 }, { "panorama", -33.4483, 149.5547 },
        { "road atlanta", 34.1458, -83.8167 }, { "atlanta", 34.1458, -83.8167 },
        { "road america", 43.7972, -87.9894 }, { "watkins", 42.3369, -76.9272 },
        { "laguna", 36.5844, -121.7533 },  { "long beach", 33.7650, -118.1892 },
        { "indianapolis", 39.7950, -86.2347 }, { "indy", 39.7950, -86.2347 },
        { "americas", 30.1328, -97.6411 }, { "cota", 30.1328, -97.6411 },
        { "interlagos", -23.7036, -46.6997 }, { "portimao", 37.2272, -8.6267 }, { "algarve", 37.2272, -8.6267 },
        { "estoril", 38.7506, -9.3942 },   { "barcelona", 41.5700, 2.2611 },  { "catalunya", 41.5700, 2.2611 },
        { "jerez", 36.7083, -6.0342 },     { "aragon", 41.0786, -0.2072 },    { "valencia", 39.4856, -0.6281 },
        { "paul ricard", 43.2506, 5.7917 }, { "ricard", 43.2506, 5.7917 },   { "castellet", 43.2506, 5.7917 },
        { "magny", 46.8642, 3.1636 },      { "zandvoort", 52.3888, 4.5409 },  { "zolder", 50.9906, 5.2567 },
        { "hockenheim", 49.3278, 8.5656 }, { "sachsenring", 50.7917, 12.6878 }, { "lausitz", 51.5336, 13.9256 },
        { "red bull ring", 47.2197, 14.7647 }, { "spielberg", 47.2197, 14.7647 }, { "hungaroring", 47.5789, 19.2486 },
        { "brno", 49.2050, 16.4447 },      { "slovakia", 48.0528, 17.5711 },
        { "suzuka", 34.8431, 136.5411 },   { "fuji", 35.3717, 138.9256 },     { "motegi", 36.5328, 140.2278 },
        { "sepang", 2.7608, 101.7381 },    { "shanghai", 31.3389, 121.2197 }, { "bahrain", 26.0325, 50.5106 },
        { "sakhir", 26.0325, 50.5106 },    { "losail", 25.4900, 51.4542 },    { "qatar", 25.4900, 51.4542 },
        { "yas", 24.4672, 54.6031 },       { "kyalami", -25.9894, 28.0767 },
        { "mosport", 44.0481, -78.6756 },  { "bowmanville", 44.0481, -78.6756 }, { "montreal", 45.5000, -73.5228 },
        { "gilles", 45.5000, -73.5228 },   { "mid-ohio", 40.6892, -82.6364 }, { "mid ohio", 40.6892, -82.6364 },
        { "lime rock", 41.9283, -73.3814 }, { "virginia", 36.5681, -79.2069 },
        { "sonoma", 38.1611, -122.4547 },  { "barber", 33.5317, -86.6194 },   { "phillip island", -38.5047, 145.2311 },
        { "mount panorama", -33.4483, 149.5547 }, { "adelaide", -34.9272, 138.6189 }, { "macau", 22.2008, 113.5492 },
        { "monaco", 43.7347, 7.4206 },     { "baku", 40.3725, 49.8533 },      { "singapore", 1.2914, 103.8644 },
    };

    std::mutex s_mx;
    std::thread s_thread;
    std::atomic<bool> s_stop{ false };
    std::atomic<bool> s_started{ false };
    std::atomic<bool> s_gameRefetch{ true };   // set by Reset: fetch the game forecast again (review F142)

    // Inputs from scoring.
    char s_track[64] = {};
    long s_session = 0;
    double s_curET = 0.0, s_endET = 0.0;

    // GAME results: five points (start fraction, sky, temp, rain chance 0..1).
    struct Node { float frac; int sky; float temp; float rain; };
    Node s_game[5];
    bool s_gameValid = false;
    // Status lines are always string literals: handed to the render thread
    // as a plain pointer, nothing is copied or allocated per frame.
    const char* s_gameStatus = "waiting";

    // REAL results: hourly, indexed from the fetch hour. Open-Meteo's rain
    // fields describe the preceding hour, so one extra hour is fetched and
    // hour h's rain is read from slot h+1 (review F137).
    const int kHours = 13;
    float s_realRain[kHours], s_realMm[kHours], s_realTemp[kHours];
    int s_realCount = 0;
    time_t s_realHour0 = 0;    // UTC time of s_real*[0]
    const char* s_realStatus = "waiting";
    char s_realTrack[64] = {};

    bool HttpGet(const wchar_t* host, INTERNET_PORT port, bool https, const std::wstring& path, std::string& body)
    {
        body.clear();
        bool ok = false;
        HINTERNET ses = WinHttpOpen(L"rF2SkrOverlay", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) return false;
        WinHttpSetTimeouts(ses, 3000, 3000, 5000, 5000);
        HINTERNET con = WinHttpConnect(ses, host, port, 0);
        HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(req, nullptr))
        {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && body.size() < 2000000)
            {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
                body.append(chunk.data(), got);
            }
            ok = code == 200 && !body.empty();
        }
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return ok;
    }

    // Tiny JSON helpers: enough for these two fixed responses.
    size_t FindKey(const std::string& s, const char* key, size_t from, size_t to = std::string::npos)
    {
        const std::string k = std::string("\"") + key + "\"";
        const size_t p = s.find(k, from);
        return (p == std::string::npos || (to != std::string::npos && p >= to)) ? std::string::npos : p + k.size();
    }
    bool NumberAfter(const std::string& s, size_t pos, double& out)
    {
        while (pos < s.size() && (s[pos] == ':' || s[pos] == ' ' || s[pos] == '"' || s[pos] == '\t')) ++pos;
        char* end = nullptr;
        out = strtod(s.c_str() + pos, &end);
        return end && end != s.c_str() + pos;
    }
    // Numbers of the JSON array that follows `key`, from `from`.
    int ArrayAfter(const std::string& s, const char* key, size_t from, double* out, int max)
    {
        size_t p = FindKey(s, key, from);
        if (p == std::string::npos) return 0;
        p = s.find('[', p);
        const size_t e = s.find(']', p);
        if (p == std::string::npos || e == std::string::npos) return 0;
        int n = 0;
        ++p;
        while (p < e && n < max)
        {
            while (p < e && (s[p] == ' ' || s[p] == ',')) ++p;
            if (s.compare(p, 4, "null") == 0) { out[n++] = NAN; p += 4; continue; }
            char* end = nullptr;
            const double v = strtod(s.c_str() + p, &end);
            if (!end || end == s.c_str() + p) break;
            out[n++] = v;
            p = static_cast<size_t>(end - s.c_str());
        }
        return n;
    }

    // End of the JSON object that starts at the first '{' at or after pos
    // (index of its closing '}'), or npos.
    size_t ObjectEnd(const std::string& s, size_t pos)
    {
        pos = s.find('{', pos);
        if (pos == std::string::npos) return pos;
        int depth = 0;
        bool str = false;
        for (size_t i = pos; i < s.size(); ++i)
        {
            const char c = s[i];
            if (str) { if (c == '\\') ++i; else if (c == '"') str = false; continue; }
            if (c == '"') str = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) return i;
        }
        return std::string::npos;
    }

    // Network values are untrusted: non-finite -> fallback, else clamped to
    // a physical range so a broken reply cannot overflow a format (review F274).
    float Clamp(double v, double lo, double hi, double fallback)
    {
        if (!std::isfinite(v)) v = fallback;
        return static_cast<float>(v < lo ? lo : (v > hi ? hi : v));
    }

    void TrackKey(const char* prefix, const char* track, char* out, size_t outSize)
    {
        strcpy_s(out, outSize, prefix);
        size_t n = strlen(out);
        for (const char* p = track; *p && n + 1 < outSize; ++p)
        {
            const unsigned char c = static_cast<unsigned char>(*p);
            out[n++] = isalnum(c) ? static_cast<char>(c) : '_';
        }
        out[n] = '\0';
        // review F420: non-ASCII names all became "___"; add a short hash
        // of the original so they don't share a key (ASCII names unchanged).
        bool lossy = false;
        unsigned int hash = 2166136261u;
        for (const char* p = track; *p; ++p)
        {
            const unsigned char c = static_cast<unsigned char>(*p);
            hash = (hash ^ c) * 16777619u;
            if (c >= 0x80 || c < 0x20) lossy = true;
        }
        if (lossy && n + 8 <= outSize)
            sprintf_s(out + n, outSize - n, "_%06x", hash & 0xFFFFFFu);
    }

    bool Coords(const char* track, double& lat, double& lon)
    {
        char key[96];
        TrackKey("CoordsLat_", track, key, sizeof(key));
        const float iLat = Config::ReadFloatLive("Conditions", key, 999.0f);
        TrackKey("CoordsLon_", track, key, sizeof(key));
        const float iLon = Config::ReadFloatLive("Conditions", key, 999.0f);
        if (iLat < 900.0f && iLon < 900.0f) { lat = iLat; lon = iLon; return true; }
        char low[64];
        size_t i = 0;
        for (; track[i] && i + 1 < sizeof(low); ++i) low[i] = static_cast<char>(tolower(static_cast<unsigned char>(track[i])));
        low[i] = '\0';
        for (const Circuit& c : kCircuits)
            if (strstr(low, c.key)) { lat = c.lat; lon = c.lon; return true; }
        return false;
    }

    // True when the game's forecast was read.
    bool FetchGame()
    {
        long session;
        { std::lock_guard<std::mutex> lk(s_mx); session = s_session; }
        // Warm-up (9) runs just before the race; the web API has only
        // PRACTICE / QUALIFY / RACE blocks (review F373).
        const char* name = session >= 9 ? "RACE" : session >= 5 ? "QUALIFY" : "PRACTICE";
        std::string body;
        if (!HttpGet(L"localhost", 5397, false, L"/rest/sessions/weather", body))
        {
            std::lock_guard<std::mutex> lk(s_mx);
            s_gameValid = false;
            s_gameStatus = "game web API not reachable";
            return false;
        }
        static const char* const kNodes[5] = { "START", "NODE_25", "NODE_50", "NODE_75", "FINISH" };
        static const float kFrac[5] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f };
        Node nodes[5];
        bool ok = true;
        // rF2 lists the sessions and the points in no fixed order (NODE_25
        // can come before START), so each object is bounded by its braces.
        const size_t sess = FindKey(body, name, 0);
        const size_t sessEnd = sess == std::string::npos ? sess : ObjectEnd(body, sess);
        if (sess == std::string::npos || sessEnd == std::string::npos) ok = false;
        for (int i = 0; ok && i < 5; ++i)
        {
            const size_t n = FindKey(body, kNodes[i], sess, sessEnd);
            const size_t next = n == std::string::npos ? n : ObjectEnd(body, n);
            double sky = 0, temp = 0, rain = 0;
            size_t p;
            ok = n != std::string::npos
              && (p = FindKey(body, "WNV_SKY", n, next)) != std::string::npos && (p = FindKey(body, "currentValue", p, next)) != std::string::npos && NumberAfter(body, p, sky)
              && (p = FindKey(body, "WNV_TEMPERATURE", n, next)) != std::string::npos && (p = FindKey(body, "currentValue", p, next)) != std::string::npos && NumberAfter(body, p, temp)
              && (p = FindKey(body, "WNV_RAIN_CHANCE", n, next)) != std::string::npos && (p = FindKey(body, "currentValue", p, next)) != std::string::npos && NumberAfter(body, p, rain);
            nodes[i] = { kFrac[i], static_cast<int>(Clamp(sky, 0.0, 10.0, 0.0)), Clamp(temp, -60.0, 60.0, 0.0),
                         Clamp(rain, 0.0, 100.0, 0.0) * 0.01f };
        }
        static bool logged = false;
        if (!logged) { logged = true; Log::Write("Weather: game forecast (%s) %s, %u bytes", name, ok ? "read" : "not understood", static_cast<unsigned>(body.size())); }
        std::lock_guard<std::mutex> lk(s_mx);
        s_gameValid = ok;
        s_gameStatus = ok ? "" : "no forecast from game";
        if (ok) for (int i = 0; i < 5; ++i) s_game[i] = nodes[i];
        return ok;
    }

    // True when a forecast was stored (or the track has no coordinates,
    // which a quick retry would not change).
    bool FetchReal()
    {
        char track[64];
        { std::lock_guard<std::mutex> lk(s_mx); strcpy_s(track, s_track); }
        if (!track[0]) return true;
        double lat, lon;
        if (!Coords(track, lat, lon))
        {
            static char loggedFor[64] = {};
            if (strcmp(loggedFor, track)) { strcpy_s(loggedFor, track); Log::Write("Weather: no coordinates for track '%s' (add CoordsLat_/CoordsLon_ under [Conditions])", track); }
            std::lock_guard<std::mutex> lk(s_mx);
            s_realCount = 0;
            s_realStatus = "track not in list (see INI)";
            strcpy_s(s_realTrack, track);
            return true;
        }
        wchar_t path[256];
        swprintf_s(path, L"/v1/forecast?latitude=%.4f&longitude=%.4f&hourly=precipitation_probability,precipitation,temperature_2m&forecast_hours=%d&timezone=GMT&timeformat=unixtime",
                   lat, lon, kHours);
        std::string body;
        const bool got = HttpGet(L"api.open-meteo.com", INTERNET_DEFAULT_HTTPS_PORT, true, path, body);
        // Doubles: unix seconds do not fit a float's 24 bits (review F406).
        double t0[kHours], rain[kHours], mm[kHours], temp[kHours];
        int n = 0;
        if (got)
        {
            const size_t h = FindKey(body, "hourly", 0);
            if (h != std::string::npos)
            {
                const int nt = ArrayAfter(body, "time", h, t0, kHours);
                const int nr = ArrayAfter(body, "precipitation_probability", h, rain, kHours);
                const int nm = ArrayAfter(body, "precipitation", FindKey(body, "precipitation_probability", h), mm, kHours);
                const int nc = ArrayAfter(body, "temperature_2m", h, temp, kHours);
                n = nt < nr ? nt : nr;
                if (nm < n) n = nm;
                if (nc < n) n = nc;
                // The first hour must be a real time near now (+/- 2 days).
                const double nowT = static_cast<double>(time(nullptr));
                if (n > 0 && !(std::isfinite(t0[0]) && fabs(t0[0] - nowT) < 2.0 * 86400.0)) n = 0;
            }
        }
        Log::Write("Weather: real forecast for '%s' (%.3f, %.3f): %s, %d hours", track, lat, lon, got ? "ok" : "request failed", n);
        std::lock_guard<std::mutex> lk(s_mx);
        // A failed fetch for a new track must not leave the previous track's
        // forecast showing under the new name (review F140).
        const bool otherTrack = strcmp(s_realTrack, track) != 0;
        strcpy_s(s_realTrack, track);
        if (n > 0)
        {
            s_realCount = n;
            s_realHour0 = static_cast<time_t>(t0[0]);
            for (int i = 0; i < n; ++i)
            {
                s_realRain[i] = Clamp(rain[i], 0.0, 100.0, 0.0) * 0.01f;
                s_realMm[i] = Clamp(mm[i], 0.0, 500.0, 0.0);
                s_realTemp[i] = Clamp(temp[i], -60.0, 60.0, 0.0);
            }
            s_realStatus = "";
        }
        else
        {
            if (otherTrack) s_realCount = 0;
            if (s_realCount == 0) s_realStatus = got ? "forecast not understood" : "no internet / service down";
        }
        return n > 0;
    }

    void ThreadBody();
    // An exception must not leave the thread (std::terminate; review F271).
    void ThreadMain()
    {
        try { ThreadBody(); }
        catch (...) {}
    }

    void ThreadBody()
    {
        const ULONGLONG kQuarter = 15ull * 60ull * 1000ull;
        ULONGLONG nextGame = 0, nextReal = 0, realRetry = 60000;
        char lastTrack[64] = {};
        while (!s_stop.load())
        {
            const ULONGLONG now = GetTickCount64();
            char track[64];
            { std::lock_guard<std::mutex> lk(s_mx); strcpy_s(track, s_track); }
            // Only while in a session (Reset clears the track), so nothing is
            // polled in the menus (review F142).
            if (s_gameRefetch.exchange(false)) nextGame = 0;
            if (g_conditionsConfig.enabled && track[0] && now >= nextGame)
            {
                // Set per session: once read only a slow re-check, until then
                // a retry every 30 s.
                nextGame = now + (FetchGame() ? 10ull * 60ull * 1000ull : 30000ull);
            }
            const bool trackChanged = strcmp(track, lastTrack) != 0;
            if (g_conditionsConfig.enabled && g_conditionsConfig.realWeather && track[0] && (now >= nextReal || trackChanged))
            {
                strcpy_s(lastTrack, track);
                if (FetchReal())
                {
                    realRetry = 60000;
                    nextReal = now + kQuarter;   // Open-Meteo updates hourly; every 15 min is plenty
                }
                else
                {
                    // Failed: retry after 1 min, backing off to 15 (review F140).
                    nextReal = now + realRetry;
                    realRetry = realRetry * 2 > kQuarter ? kQuarter : realRetry * 2;
                }
            }
            for (int i = 0; i < 10 && !s_stop.load(); ++i) Sleep(100);
        }
    }
}

void Weather::OnScoring(const ScoringInfoV01& info)
{
    {
        std::lock_guard<std::mutex> lk(s_mx);
        strncpy_s(s_track, info.mTrackName, _TRUNCATE);
        s_session = info.mSession;
        s_curET = info.mCurrentET;
        s_endET = info.mEndET;
    }
    if (!s_started.exchange(true))
    {
        s_stop = false;
        s_thread = std::thread(ThreadMain);
        MakeThreadEfficient(static_cast<HANDLE>(s_thread.native_handle()));
    }
}

void Weather::Reset()
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_gameValid = false;
    s_gameStatus = "waiting";
    s_track[0] = '\0';   // out of the session: no fetching until scoring sets it again (review F142)
    s_gameRefetch = true;
}

void Weather::Shutdown()
{
    if (!s_started.load()) return;
    s_stop = true;
    if (s_thread.joinable())
    {
        // At most 3 s (review F170): a request stuck in WinHTTP must not
        // hold the game's exit. If it is still running, keep the DLL loaded
        // (pinned) so the thread never runs unmapped code, and let it go.
        const DWORD w = WaitForSingleObject(static_cast<HANDLE>(s_thread.native_handle()), 3000);
        if (w == WAIT_OBJECT_0)
            s_thread.join();
        else
        {
            HMODULE h = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&Weather::Shutdown), &h);
            s_thread.detach();
            Log::Write("Weather: thread still busy at shutdown -- left running, DLL pinned");
        }
    }
    s_started = false;
}

int Weather::Game(GamePoint* out, int max, const char*& status)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_gameValid) { status = s_gameStatus; return 0; }
    if (s_endET <= 0.0) { status = "session has no end time"; return 0; }
    int n = 0;
    for (int i = 0; i < 5 && n < max; ++i)
    {
        const float mins = static_cast<float>((s_game[i].frac * s_endET - s_curET) / 60.0);
        // The point now in force (the last one already passed) shows as "now".
        const bool current = mins <= 0.0f && (i == 4 || (s_game[i + 1].frac * s_endET - s_curET) > 0.0);
        if (mins > 0.0f || current)
            out[n++] = { mins > 0.0f ? mins : 0.0f, s_game[i].sky, s_game[i].temp, s_game[i].rain };
    }
    status = n ? "" : "no points left";
    return n;
}

int Weather::Real(RealPoint* out, int max, const char*& status)
{
    if (!g_conditionsConfig.realWeather) { status = "off"; return 0; }
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_realCount == 0 || strcmp(s_realTrack, s_track) != 0) { status = s_realStatus; return 0; }
    const time_t now = time(nullptr);
    const long long since = static_cast<long long>(now) - static_cast<long long>(s_realHour0);
    const int skip = since < 0 ? 0 : static_cast<int>(since / 3600);   // hours since the fetch started
    int n = 0;
    // Slot i's temperature is at its hour start; its rain and mm cover the
    // hour before, so the hour starting at slot i reads rain from i+1 (review F137).
    for (int i = skip; i + 1 < s_realCount && n < max; ++i)
        out[n++] = { i - skip, s_realRain[i + 1], s_realMm[i + 1], s_realTemp[i] };
    status = n ? "" : "forecast out of date";
    return n;
}

const char* Weather::SkyName(int sky)
{
    static const char* const k[] = { "clear", "light cloud", "part cloudy", "mostly cloudy", "overcast",
                                     "drizzle", "light rain", "light rain", "rain", "heavy rain", "storm" };
    return sky >= 0 && sky <= 10 ? k[sky] : "?";
}
