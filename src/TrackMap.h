#pragma once
#include <Windows.h>

// Overhead track map: the circuit outline with a dot and short name per car.
//
// WHERE THE OUTLINE COMES FROM, in this order:
//  1. This plugin's own recordings, in rF2SkrOverlayPublic_TrackMaps\ beside the DLL.
//  2. TinyPedal's saved maps (TrackMapConfig::tinyPedalMapDir), when TinyPedal
//     is installed (often dozens of circuits).
// A circuit found in neither is RECORDED: the player's next clean, pit-free
// lap is captured from scoring (~5Hz) and becomes the map at the line. The
// file itself is written on the Persist thread (see FlushIfDirty), like every
// other disk write in this plugin. Recordings use TinyPedal's own format, so
// one loader reads both:
//
//   <polyline id="map">   world (x, -z) of the path -- TinyPedal's convention
//   <polyline id="dist">  (lapDistance, elevation) for the SAME index
//   <desc>169,348</desc>  sector 1 / sector 2 boundaries as INDICES into both
//
// Cars are placed by LAP DISTANCE along that same array, never by world
// position, so dots and outline share one space by construction whatever
// convention the file was recorded in.
//
// ROTATION AND MIRROR, PER TRACK. rFactor 2's world axes bear no relation to
// how a circuit is drawn in a track guide, so a map often comes out sideways
// -- and the angle that stands Monza up leaves Spa on its side, so the angle
// belongs to the circuit: Rotation_<track> / Mirror_<track> in the INI's
// [TrackMap] section, set with a slider and a flip button shown under the map
// in layout mode. The mirror exists because rFactor 2's coordinate system is
// left-handed: a reversed map can never be fixed by rotating, only by a flip.
// Rotation is baked into the points once when it changes, and the bounding box
// recomputed from the rotated result so a turned map still fits its box.
class Renderer;
// Global scope: see the note in DriverAbbrev.h.
struct ScoringInfoV01;

namespace TrackMap
{
    // Scoring thread (~5Hz). Notes the track name and every car's lap
    // distance, and runs the recorder when the track has no map. The file
    // load itself runs on a worker thread Draw starts, so no game callback
    // (and no frame) ever waits on the disk.
    void OnScoring(const ScoringInfoV01& info);

    void Draw(Renderer& renderer, int screenWidth, int screenHeight);

    // The rotation slider and mirror button, drawn only in layout mode, under
    // the map. Needs the swap chain's window to hit-test the cursor. Call
    // AFTER Draw(), so the controls sit over the map's frame and act on the
    // rectangle this frame actually used.
    void DrawLayoutControls(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd);

    // Writes a freshly recorded map to disk, if there is one waiting. Called
    // from the Persist thread only (at the line and out of the car, with the
    // plugin's other FlushIfDirty() calls); takes the recording under the
    // module's lock and writes it after releasing it.
    void FlushIfDirty();
    // Plugin shutdown: waits (up to 3 s) for a map-load worker still
    // running, so the DLL is never unloaded under it.
    void Shutdown();

    // EndSession. Drops the cars and asks the render thread to drop the map.
    void Clear();

    // For the options list (render thread, like Draw). Rotation and mirror
    // act on the circuit loaded now and are saved for it, the same as the
    // layout-mode slider and button.
    bool HasMap();
    float RotationDegrees();
    void SetRotationDegrees(float degrees, bool persist);
    bool Mirrored();
    void ToggleMirror();
    // Re-record the loaded circuit's map from the next clean lap, replacing
    // a badly drawn one; the old map stays until the new one is in. Calling
    // it again while waiting cancels. A re-recorded map is saved as this
    // plugin's own file, which takes priority over TinyPedal's.
    void ToggleRerecord();
    bool Rerecording();
}
