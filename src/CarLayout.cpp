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
        { "PitLimiter", "EngageWarningXFraction", &g_pitLimiterConfig.engageWarningXFraction },
        { "PitLimiter", "EngageWarningYFraction", &g_pitLimiterConfig.engageWarningYFraction },
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
    // under g_lock: Poll runs on the render thread, FlushIfDirty on scoring).
    std::string g_currentKey;
    std::vector<float> g_saved;

    void LayoutsPath(char* out, size_t outSize)
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LayoutsPath), &self);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(self, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) *(slash + 1) = 0;
        sprintf_s(out, outSize, "%srF2SkrOverlayPublic_Layouts.ini", path);
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

    // Writes the values that differ from 'previous' (all of them when it is
    // empty) into the car's section. Caller holds g_lock.
    void SaveSection(const std::string& key, const std::vector<float>& now, const std::vector<float>& previous)
    {
        char path[MAX_PATH];
        LayoutsPath(path, sizeof(path));
        const bool all = previous.size() != now.size();
        for (int i = 0; i < kValueCount; ++i)
        {
            if (!all && previous[i] == now[i])
                continue;
            char k[96], v[32];
            ItemKey(i, k, sizeof(k));
            sprintf_s(v, "%.4f", now[i]);
            WritePrivateProfileStringA(key.c_str(), k, v, path);
        }
    }

    bool SectionExists(const std::string& key)
    {
        char path[MAX_PATH];
        LayoutsPath(path, sizeof(path));
        char buf[16];
        return GetPrivateProfileSectionA(key.c_str(), buf, sizeof(buf), path) > 0;
    }

    // Puts the car's saved layout live, and into the main INI so it is also
    // what loads next time. Values the section lacks are left as they are.
    void ApplySection(const std::string& key)
    {
        char path[MAX_PATH];
        LayoutsPath(path, sizeof(path));
        for (int i = 0; i < kValueCount; ++i)
        {
            char k[96], v[32];
            ItemKey(i, k, sizeof(k));
            if (GetPrivateProfileStringA(key.c_str(), k, "", v, sizeof(v), path) == 0)
                continue;
            const float value = static_cast<float>(atof(v));
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
    char vehicleName[64], vehicleClass[32], plrFileName[64];
    AcquireSRWLockExclusive(&g_lock);
    const bool changed = g_carChanged;
    g_carChanged = false;
    strcpy_s(vehicleName, g_vehicleName);
    strcpy_s(vehicleClass, g_vehicleClass);
    strcpy_s(plrFileName, g_plrFileName);
    ReleaseSRWLockExclusive(&g_lock);
    if (!changed || !vehicleClass[0])
        return;

    const std::string key = ResolveKey(vehicleClass, plrFileName);

    AcquireSRWLockExclusive(&g_lock);
    if (key == g_currentKey)
    {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    std::vector<float> now;
    Snapshot(now);
    // The car being left keeps what was on screen for it.
    if (!g_currentKey.empty())
        SaveSection(g_currentKey, now, g_saved);

    const bool known = SectionExists(key);
    if (known)
        ApplySection(key);
    else
        SaveSection(key, now, std::vector<float>()); // a new car starts from the current layout
    g_currentKey = key;
    Snapshot(g_saved);
    ReleaseSRWLockExclusive(&g_lock);

    Log::Write("CarLayout: '%s' (%s) -> layout [%s]%s", vehicleName, vehicleClass, key.c_str(),
               known ? "" : " (new, from the current layout)");
    char msg[128];
    sprintf_s(msg, "HUD layout: %s", key.c_str() + key.find(':') + 1);
    Shortcuts::ShowMessage(msg, 3.0);
}

void CarLayout::FlushIfDirty()
{
    AcquireSRWLockExclusive(&g_lock);
    if (!g_currentKey.empty())
    {
        std::vector<float> now;
        Snapshot(now);
        if (now != g_saved)
        {
            SaveSection(g_currentKey, now, g_saved);
            g_saved.swap(now);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}
