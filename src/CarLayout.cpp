#include "CarLayout.h"
#include "InternalsPlugin.hpp"
#include "Config.h"
#include "Shortcuts.h"
#include "Log.h"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // Every panel position and size the layout mode can change, by the INI
    // section/key it is saved under (the same ones Layout::Register gets).
    struct FloatItem { const char* section; const char* key; float* value; };
    const FloatItem kFloats[] =
    {
        { "Radar", "CenterXFraction", &g_radarConfig.centerXFraction },
        { "Radar", "CenterYFraction", &g_radarConfig.centerYFraction },
        { "Radar", "Scale", &g_radarConfig.scale },
        { "PedalTrace", "CenterXFraction", &g_pedalConfig.centerXFraction },
        { "PedalTrace", "CenterYFraction", &g_pedalConfig.centerYFraction },
        { "PedalTrace", "Scale", &g_pedalConfig.scale },
        { "GearDisplay", "CenterXFraction", &g_gearDisplayConfig.centerXFraction },
        { "GearDisplay", "CenterYFraction", &g_gearDisplayConfig.centerYFraction },
        { "GearDisplay", "Scale", &g_gearDisplayConfig.scale },
        { "GearDisplay", "TyreLockupPosXFraction", &g_gearDisplayConfig.tyreLockupPosXFraction },
        { "GearDisplay", "TyreLockupPosYFraction", &g_gearDisplayConfig.tyreLockupPosYFraction },
        { "GearDisplay", "TyreLockupScale", &g_gearDisplayConfig.tyreLockupScale },
        { "GearDisplay", "ShiftStatsPosXFraction", &g_gearDisplayConfig.shiftStatsPosXFraction },
        { "GearDisplay", "ShiftStatsPosYFraction", &g_gearDisplayConfig.shiftStatsPosYFraction },
        { "GearDisplay", "ShiftStatsScale", &g_gearDisplayConfig.shiftStatsScale },
        { "PitLimiter", "BarXFraction", &g_pitLimiterConfig.barXFraction },
        { "PitLimiter", "BarYFraction", &g_pitLimiterConfig.barYFraction },
        { "PitLimiter", "BarScale", &g_pitLimiterConfig.barScale },
        { "FuelSave", "CenterXFraction", &g_fuelSaveConfig.centerXFraction },
        { "FuelSave", "CenterYFraction", &g_fuelSaveConfig.centerYFraction },
        { "FuelSave", "Scale", &g_fuelSaveConfig.scale },
        { "TyreSave", "CenterXFraction", &g_tyreSaveConfig.centerXFraction },
        { "TyreSave", "CenterYFraction", &g_tyreSaveConfig.centerYFraction },
        { "TyreSave", "Scale", &g_tyreSaveConfig.scale },
        { "EngineTemps", "CenterXFraction", &g_engineTempsConfig.centerXFraction },
        { "EngineTemps", "CenterYFraction", &g_engineTempsConfig.centerYFraction },
        { "EngineTemps", "Scale", &g_engineTempsConfig.scale },
        { "FfbLevel", "CenterXFraction", &g_ffbLevelConfig.centerXFraction },
        { "FfbLevel", "CenterYFraction", &g_ffbLevelConfig.centerYFraction },
        { "FfbLevel", "Scale", &g_ffbLevelConfig.scale },
        { "Conditions", "CenterXFraction", &g_conditionsConfig.centerXFraction },
        { "Conditions", "CenterYFraction", &g_conditionsConfig.centerYFraction },
        { "Conditions", "Scale", &g_conditionsConfig.scale },
        { "LapDelta", "CenterXFraction", &g_lapDeltaConfig.centerXFraction },
        { "LapDelta", "CenterYFraction", &g_lapDeltaConfig.centerYFraction },
        { "LapDelta", "Scale", &g_lapDeltaConfig.scale },
        { "LapDelta", "BestCenterXFraction", &g_lapDeltaConfig.bestCenterXFraction },
        { "LapDelta", "BestCenterYFraction", &g_lapDeltaConfig.bestCenterYFraction },
        { "LapDelta", "DrsCenterXFraction", &g_lapDeltaConfig.drsCenterXFraction },
        { "LapDelta", "DrsCenterYFraction", &g_lapDeltaConfig.drsCenterYFraction },
        { "LapDelta", "DrsScale", &g_lapDeltaConfig.drsScale },
        { "MiniSector", "CenterXFraction", &g_miniSectorConfig.centerXFraction },
        { "MiniSector", "CenterYFraction", &g_miniSectorConfig.centerYFraction },
        { "MiniSector", "Scale", &g_miniSectorConfig.scale },
        { "BrakeCue", "ReactionStatsCenterXFraction", &g_brakeCueConfig.reactionStatsCenterXFraction },
        { "BrakeCue", "ReactionStatsCenterYFraction", &g_brakeCueConfig.reactionStatsCenterYFraction },
        { "BrakeCue", "ReactionStatsScale", &g_brakeCueConfig.reactionStatsScale },
        { "FrameLatency", "CpuGpuPosXFraction", &g_frameLatencyConfig.cpuGpuPosXFraction },
        { "FrameLatency", "CpuGpuPosYFraction", &g_frameLatencyConfig.cpuGpuPosYFraction },
        { "FrameLatency", "Scale", &g_frameLatencyConfig.scale },
        { "Clock", "PosXFraction", &g_clockConfig.posXFraction },
        { "Clock", "PosYFraction", &g_clockConfig.posYFraction },
        { "Clock", "Scale", &g_clockConfig.scale },
        { "DllCost", "PosXFraction", &g_dllCostConfig.posXFraction },
        { "DllCost", "PosYFraction", &g_dllCostConfig.posYFraction },
        { "DllCost", "Scale", &g_dllCostConfig.scale },
        { "TyreTemps", "PosXFraction", &g_tyreTempsConfig.posXFraction },
        { "TyreTemps", "PosYFraction", &g_tyreTempsConfig.posYFraction },
        { "TyreTemps", "Scale", &g_tyreTempsConfig.scale },
        { "FfbGraph", "PosXFraction", &g_ffbGraphConfig.posXFraction },
        { "FfbGraph", "PosYFraction", &g_ffbGraphConfig.posYFraction },
        { "FfbGraph", "Scale", &g_ffbGraphConfig.scale },
        { "TrackMap", "PosXFraction", &g_trackMapConfig.posXFraction },
        { "TrackMap", "PosYFraction", &g_trackMapConfig.posYFraction },
        { "TrackMap", "Scale", &g_trackMapConfig.scale },
        { "RelativeTiming", "PosXFraction", &g_relativeTimingConfig.posXFraction },
        { "RelativeTiming", "PosYFraction", &g_relativeTimingConfig.posYFraction },
        { "RelativeTiming", "Scale", &g_relativeTimingConfig.scale },
        { "RelativeTiming", "GapBoxPosXFraction", &g_relativeTimingConfig.gapPosXFraction },
        { "RelativeTiming", "GapBoxPosYFraction", &g_relativeTimingConfig.gapPosYFraction },
        { "RelativeTiming", "GapBoxScale", &g_relativeTimingConfig.gapScale },
        { "FuelTyrePanel_Fuel", "PosXFraction", &g_fuelTyrePanelConfig.fuelPosXFraction },
        { "FuelTyrePanel_Fuel", "PosYFraction", &g_fuelTyrePanelConfig.fuelPosYFraction },
        { "FuelTyrePanel_Fuel", "Scale", &g_fuelTyrePanelConfig.fuelScale },
        { "FuelTyrePanel_Tyres", "PosXFraction", &g_fuelTyrePanelConfig.tyresPosXFraction },
        { "FuelTyrePanel_Tyres", "PosYFraction", &g_fuelTyrePanelConfig.tyresPosYFraction },
        { "FuelTyrePanel_Tyres", "Scale", &g_fuelTyrePanelConfig.tyresScale },
        { "FuelTyrePanel_BrakeBias", "PosXFraction", &g_fuelTyrePanelConfig.brakeBiasPosXFraction },
        { "FuelTyrePanel_BrakeBias", "PosYFraction", &g_fuelTyrePanelConfig.brakeBiasPosYFraction },
        { "FuelTyrePanel_BrakeBias", "Scale", &g_fuelTyrePanelConfig.brakeBiasScale },
        { "CoverBox", "Box1PosXFraction", &g_coverBoxConfig.posXFraction[0] },
        { "CoverBox", "Box1PosYFraction", &g_coverBoxConfig.posYFraction[0] },
        { "CoverBox", "Box1WidthPixels", &g_coverBoxConfig.widthPx[0] },
        { "CoverBox", "Box1HeightPixels", &g_coverBoxConfig.heightPx[0] },
        { "CoverBox", "Box2PosXFraction", &g_coverBoxConfig.posXFraction[1] },
        { "CoverBox", "Box2PosYFraction", &g_coverBoxConfig.posYFraction[1] },
        { "CoverBox", "Box2WidthPixels", &g_coverBoxConfig.widthPx[1] },
        { "CoverBox", "Box2HeightPixels", &g_coverBoxConfig.heightPx[1] },
        { "CoverBox", "Box3PosXFraction", &g_coverBoxConfig.posXFraction[2] },
        { "CoverBox", "Box3PosYFraction", &g_coverBoxConfig.posYFraction[2] },
        { "CoverBox", "Box3WidthPixels", &g_coverBoxConfig.widthPx[2] },
        { "CoverBox", "Box3HeightPixels", &g_coverBoxConfig.heightPx[2] },
        { "CoverBox", "Box4PosXFraction", &g_coverBoxConfig.posXFraction[3] },
        { "CoverBox", "Box4PosYFraction", &g_coverBoxConfig.posYFraction[3] },
        { "CoverBox", "Box4WidthPixels", &g_coverBoxConfig.widthPx[3] },
        { "CoverBox", "Box4HeightPixels", &g_coverBoxConfig.heightPx[3] },
        { "CoverBox", "Grey", &g_coverBoxConfig.grey },
        { "CoverBox", "Alpha", &g_coverBoxConfig.a },
    };
    const int kFloatCount = static_cast<int>(sizeof(kFloats) / sizeof(kFloats[0]));
    // Whole-number settings, saved alongside as numbers.
    struct IntItem { const char* section; const char* key; int* value; };
    const IntItem kInts[] =
    {
        { "CoverBox", "Count", &g_coverBoxConfig.count },
    };
    const int kIntCount = static_cast<int>(sizeof(kInts) / sizeof(kInts[0]));
    const int kValueCount = kFloatCount + kIntCount + Shortcuts::ElementCount;

    // Scoring -> render handoff, under g_lock.
    SRWLOCK g_lock = SRWLOCK_INIT;
    char g_vehicleName[64] = {};
    char g_vehicleClass[32] = {};
    char g_plrFileName[64] = {};
    bool g_carChanged = false;

    // The car whose layout is live, and what was last saved for it (both
    // under g_lock: Poll runs on the render thread, FlushIfDirty on the
    // Persist thread).
    std::string g_currentKey;
    std::vector<float> g_saved;
    // A car-change worker is running (also under g_lock). FlushIfDirty stays
    // out of the way meanwhile: the worker saves the car being left itself,
    // and g_currentKey is about to change.
    bool g_jobBusy = false;

    void LayoutsPath(char* out, size_t outSize)
    {
        static char s_cached[MAX_PATH] = "";
        if (s_cached[0]) { strcpy_s(out, outSize, s_cached); return; }
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LayoutsPath), &self);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(self, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) *(slash + 1) = 0;
        sprintf_s(out, outSize, "%srF2SkrOverlayPublic_Layouts.ini", path);
        strcpy_s(s_cached, out);
    }

    // rF2's root, two folders up from Bin64\Plugins\ where the DLL lives.
    void GameRoot(char* out, size_t outSize)
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&GameRoot), &self);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(self, path, MAX_PATH);
        for (int up = 0; up < 3; ++up)
        {
            char* slash = strrchr(path, '\\');
            if (!slash) break;
            *slash = 0;
        }
        sprintf_s(out, outSize, "%s\\", path);
    }

    // The car model folder from the player profile's "Vehicle File", e.g.
    // ...\Installed\Vehicles\Porsche_992_GT3_Cup_2022\1.22gtc\X.VEH ->
    // "Porsche_992_GT3_Cup_2022". Empty if it cannot be found.
    std::string ReadVehicleFolder(const char* plrFileName)
    {
        char root[MAX_PATH];
        GameRoot(root, sizeof(root));
        const char* plr = (plrFileName && plrFileName[0]) ? plrFileName : "player";
        char path[MAX_PATH];
        sprintf_s(path, "%sUserData\\%s\\%s.JSON", root, plr, plr);

        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f)
        {
            Log::Write("CarLayout: could not open '%s'", path);
            return std::string();
        }
        std::string text;
        char buf[8192];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, got);
        fclose(f);

        const char* tag = "\"Vehicle File\":\"";
        const size_t at = text.find(tag);
        if (at == std::string::npos)
            return std::string();
        const size_t start = at + strlen(tag);
        const size_t end = text.find('"', start);
        if (end == std::string::npos)
            return std::string();
        // JSON escapes each backslash; reduce to one per separator.
        std::string file;
        for (size_t i = start; i < end; ++i)
        {
            if (text[i] == '\\' && i + 1 < end && text[i + 1] == '\\')
                ++i;
            file += text[i];
        }
        std::string lower = file;
        for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        const char* marker = "\\vehicles\\";
        const size_t v = lower.find(marker);
        if (v == std::string::npos)
            return std::string();
        const size_t fStart = v + strlen(marker);
        const size_t fEnd = file.find('\\', fStart);
        return file.substr(fStart, fEnd == std::string::npos ? std::string::npos : fEnd - fStart);
    }

    // INI section names cannot hold ']' and keys cannot hold '='.
    std::string Sanitise(const std::string& s)
    {
        std::string out = s;
        for (char& c : out)
            if (c == '[' || c == ']' || c == '=' || c == ';' || c == '\r' || c == '\n') c = '_';
        return out;
    }

    void Snapshot(std::vector<float>& out)
    {
        out.resize(kValueCount);
        for (int i = 0; i < kFloatCount; ++i)
            out[i] = *kFloats[i].value;
        for (int i = 0; i < kIntCount; ++i)
            out[kFloatCount + i] = static_cast<float>(*kInts[i].value);
        for (int e = 0; e < Shortcuts::ElementCount; ++e)
            out[kFloatCount + kIntCount + e] = Shortcuts::Toggled(static_cast<Shortcuts::Element>(e)) ? 1.0f : 0.0f;
    }

    void ItemKey(int i, char* out, size_t outSize)
    {
        if (i < kFloatCount)
            sprintf_s(out, outSize, "%s.%s", kFloats[i].section, kFloats[i].key);
        else if (i < kFloatCount + kIntCount)
            sprintf_s(out, outSize, "%s.%s", kInts[i - kFloatCount].section, kInts[i - kFloatCount].key);
        else
            sprintf_s(out, outSize, "Shortcuts.%s", Shortcuts::IniKey(static_cast<Shortcuts::Element>(i - kFloatCount - kIntCount)));
    }

    void ItemNames(std::vector<std::string>& names)
    {
        names.resize(kValueCount);
        for (int i = 0; i < kValueCount; ++i)
        {
            char k[96];
            ItemKey(i, k, sizeof(k));
            names[i] = k;
        }
    }

    // Writes a car's whole section in ONE file write (WritePrivateProfile-
    // Section). It used to be one WritePrivateProfileString per changed
    // value, each rereading and rewriting the whole layouts file -- ~120 of
    // them for a new car. That call replaces the section, so every value is
    // written, and any line already there that this build does not know (a
    // key from another version) is carried over as it was. Caller holds
    // g_fileLock; never called with g_lock held.
    void SaveSection(const std::string& key, const std::vector<float>& now)
    {
        if (static_cast<int>(now.size()) != kValueCount)
            return;
        char path[MAX_PATH];
        LayoutsPath(path, sizeof(path));
        std::vector<std::string> names;
        ItemNames(names);

        std::string block;
        block.reserve(static_cast<size_t>(kValueCount) * 48);
        for (int i = 0; i < kValueCount; ++i)
        {
            char v[32];
            sprintf_s(v, "%.4f", now[i]);
            block += names[i];
            block += '=';
            block += v;
            block += '\0';
        }
        std::vector<char> old(64 * 1024);
        if (GetPrivateProfileSectionA(key.c_str(), old.data(), static_cast<DWORD>(old.size()), path) > 0)
        {
            for (const char* line = old.data(); *line; line += strlen(line) + 1)
            {
                const char* eq = strchr(line, '=');
                const std::string name = eq ? std::string(line, eq - line) : std::string(line);
                bool ours = false;
                for (const std::string& n : names)
                    if (n == name) { ours = true; break; }
                if (!ours)
                {
                    block += line;
                    block += '\0';
                }
            }
        }
        block += '\0';   // the list ends with an empty string
        if (!WritePrivateProfileSectionA(key.c_str(), block.c_str(), path))
            Log::Write("CarLayout: could not save [%s] (error %lu)", key.c_str(), GetLastError());
    }

    // Reads a car's whole section in ONE file read (GetPrivateProfileSection)
    // and matches it to the value list. It used to be one
    // GetPrivateProfileString per value -- dozens of re-opens and re-parses
    // of the file, on the render thread, at every car change.
    bool ReadSection(const std::string& key, std::vector<float>& vals, std::vector<char>& have)
    {
        vals.assign(kValueCount, 0.0f);
        have.assign(kValueCount, 0);
        char path[MAX_PATH];
        LayoutsPath(path, sizeof(path));
        std::vector<char> buf(64 * 1024);
        const DWORD got = GetPrivateProfileSectionA(key.c_str(), buf.data(), static_cast<DWORD>(buf.size()), path);
        if (got == 0)
            return false;
        std::vector<std::string> names;
        ItemNames(names);
        for (const char* line = buf.data(); *line; line += strlen(line) + 1)
        {
            const char* eq = strchr(line, '=');
            if (!eq) continue;
            const std::string name(line, eq - line);
            for (int i = 0; i < kValueCount; ++i)
                if (names[i] == name) { vals[i] = static_cast<float>(atof(eq + 1)); have[i] = 1; break; }
        }
        return true;
    }

    // Puts a car's saved layout live, and into the main INI so it is also
    // what loads next time (queued writes). Values the section lacks are left
    // as they are. Render thread.
    void ApplyValues(const std::vector<float>& vals, const std::vector<char>& have)
    {
        for (int i = 0; i < kValueCount; ++i)
        {
            if (!have[i]) continue;
            const float value = vals[i];
            if (i < kFloatCount)
            {
                if (*kFloats[i].value != value)
                {
                    *kFloats[i].value = value;
                    Config::WriteFloat(kFloats[i].section, kFloats[i].key, value);
                }
            }
            else if (i < kFloatCount + kIntCount)
            {
                const IntItem& it = kInts[i - kFloatCount];
                const int iv = static_cast<int>(value + 0.5f);
                if (*it.value != iv)
                {
                    *it.value = iv;
                    Config::WriteFloat(it.section, it.key, static_cast<float>(iv));
                }
            }
            else
            {
                Shortcuts::SetVisible(static_cast<Shortcuts::Element>(i - kFloatCount - kIntCount), value > 0.5f);
            }
        }
    }

    // "car:<model folder>" when the profile can be trusted, else "class:<class>".
    std::string ResolveKey(const char* vehicleClass, const char* plrFileName)
    {
        const std::string cls = Sanitise(vehicleClass);
        const std::string folder = Sanitise(ReadVehicleFolder(plrFileName));
        if (folder.empty())
            return "class:" + cls;

        // Each model folder remembers the class it first came with. A
        // different class now means the profile still names the previous car.
        char path[MAX_PATH];
        LayoutsPath(path, sizeof(path));
        char seen[64] = {};
        GetPrivateProfileStringA("_ModelClass", folder.c_str(), "", seen, sizeof(seen), path);
        if (seen[0] == 0)
            WritePrivateProfileStringA("_ModelClass", folder.c_str(), cls.c_str(), path);
        else if (cls != seen)
        {
            Log::Write("CarLayout: profile names '%s' (class %s) but the car is class %s -- using the class",
                       folder.c_str(), seen, cls.c_str());
            return "class:" + cls;
        }
        return "car:" + folder;
    }
}

namespace
{
    // A car change does its file work (reading the player profile, saving
    // the old car's section, reading the new one) on a worker thread; the
    // render thread only starts it and applies the result. It used to all
    // run on the render thread, a hitch of tens of ms per car change.
    SRWLOCK g_fileLock = SRWLOCK_INIT;   // the layouts file: worker vs FlushIfDirty
    // Touched only by the render thread (Poll), and by the worker while it
    // runs; Poll reads the results only once the thread has exited.
    struct Job
    {
        HANDLE thread = nullptr;
        // in
        std::string vehicleName, vehicleClass, plr, prevKey;
        std::vector<float> now, prevSaved;
        // out
        std::string key;
        bool same = false, known = false;
        std::vector<float> vals;
        std::vector<char> have;
    } g_job;

    DWORD WINAPI JobProc(LPVOID)
    {
        AcquireSRWLockExclusive(&g_fileLock);
        g_job.key = ResolveKey(g_job.vehicleClass.c_str(), g_job.plr.c_str());
        g_job.same = g_job.key == g_job.prevKey;
        if (!g_job.same)
        {
            // The car being left keeps what was on screen for it (unless
            // nothing changed since it was last saved).
            if (!g_job.prevKey.empty() && g_job.now != g_job.prevSaved)
                SaveSection(g_job.prevKey, g_job.now);
            g_job.known = ReadSection(g_job.key, g_job.vals, g_job.have);
            if (!g_job.known)
                SaveSection(g_job.key, g_job.now); // a new car starts from the current layout
        }
        ReleaseSRWLockExclusive(&g_fileLock);
        return 0;
    }
}

void CarLayout::OnScoring(const ScoringInfoV01& info)
{
    if (!info.mVehicle)
        return;
    for (long i = 0; i < info.mNumVehicles; ++i)
    {
        const VehicleScoringInfoV01& v = info.mVehicle[i];
        if (!v.mIsPlayer)
            continue;
        AcquireSRWLockExclusive(&g_lock);
        if (strncmp(g_vehicleName, v.mVehicleName, sizeof(g_vehicleName)) != 0
            || strncmp(g_vehicleClass, v.mVehicleClass, sizeof(g_vehicleClass)) != 0)
        {
            strncpy_s(g_vehicleName, v.mVehicleName, _TRUNCATE);
            strncpy_s(g_vehicleClass, v.mVehicleClass, _TRUNCATE);
            strncpy_s(g_plrFileName, info.mPlrFileName, _TRUNCATE);
            g_carChanged = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
}

void CarLayout::Poll()
{
    if (!g_job.thread)
    {
        char vehicleName[64], vehicleClass[32], plrFileName[64];
        AcquireSRWLockExclusive(&g_lock);
        const bool changed = g_carChanged;
        g_carChanged = false;
        strcpy_s(vehicleName, g_vehicleName);
        strcpy_s(vehicleClass, g_vehicleClass);
        strcpy_s(plrFileName, g_plrFileName);
        if (changed && vehicleClass[0])
        {
            g_job.vehicleName = vehicleName;
            g_job.vehicleClass = vehicleClass;
            g_job.plr = plrFileName;
            g_job.prevKey = g_currentKey;
            Snapshot(g_job.now);
            g_job.prevSaved = g_saved;
            g_job.thread = CreateThread(nullptr, 0, JobProc, nullptr, 0, nullptr);
            g_jobBusy = g_job.thread != nullptr;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // Polled, never waited for: a thread's exit can sit behind the loader
    // lock for a while during a session load, and an INFINITE wait here was
    // the frame waiting with it. The results are used once it has exited.
    if (WaitForSingleObject(g_job.thread, 0) != WAIT_OBJECT_0)
        return;                         // still working
    CloseHandle(g_job.thread);
    g_job.thread = nullptr;
    if (g_job.same)
    {
        AcquireSRWLockExclusive(&g_lock);
        g_jobBusy = false;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    const std::string key = g_job.key;
    const bool known = g_job.known;
    const char* vehicleName = g_job.vehicleName.c_str();
    const char* vehicleClass = g_job.vehicleClass.c_str();
    AcquireSRWLockExclusive(&g_lock);
    const bool firstCar = g_currentKey.empty();
    (void)firstCar;
    if (known)
        ApplyValues(g_job.vals, g_job.have);
    g_currentKey = key;
    Snapshot(g_saved);
    g_jobBusy = false;
    ReleaseSRWLockExclusive(&g_lock);

    Log::Write("CarLayout: '%s' (%s) -> layout [%s]%s", vehicleName, vehicleClass, key.c_str(),
               known ? "" : " (new, from the current layout)");
    char msg[128];
    sprintf_s(msg, "HUD layout: %s", key.c_str() + key.find(':') + 1);
    Shortcuts::ShowMessage(msg, 3.0);
}

void CarLayout::FlushIfDirty()
{
    // Persist thread. Copy what to save under g_lock (which Poll takes every
    // frame), then write with it released -- holding it across the file
    // write would make that frame wait for the disk.
    //
    // g_fileLock is taken first and held to the end: a car-change worker
    // started after the copy then waits for this write, and its own, newer
    // copy of the same car lands on top rather than under it. (Lock order is
    // always g_fileLock, then g_lock; Poll and the worker each take only one.)
    AcquireSRWLockExclusive(&g_fileLock);
    std::string key;
    std::vector<float> now;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_currentKey.empty() && !g_jobBusy)
    {
        Snapshot(now);
        if (now != g_saved)
            key = g_currentKey;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (key.empty())
    {
        ReleaseSRWLockExclusive(&g_fileLock);
        return;
    }
    SaveSection(key, now);
    ReleaseSRWLockExclusive(&g_fileLock);

    // Only remember it as saved if that car is still the live one and no
    // car change has started since (which takes over g_saved itself).
    AcquireSRWLockExclusive(&g_lock);
    if (!g_jobBusy && g_currentKey == key)
        g_saved.swap(now);
    ReleaseSRWLockExclusive(&g_lock);
}

void CarLayout::Shutdown()
{
    // Called after the Present hook is removed, so Poll no longer touches
    // the handle; the worker saves its own sections, so just let it finish.
    if (g_job.thread)
        WaitForSingleObject(g_job.thread, 3000);
}
