# Changelog

## 26.11.24.1

### Fixed
- **Black screen at startup with NVIDIA Surround** (reported by a user): the plugin no longer hooks the graphics
  while the game starts up and sets the screen mode. It starts drawing when you load into your first session, so
  nothing shows in the main menus before that.

### Changed
- **Rotary setting lists grouped** by colour: brakes and chassis, engine and TC, strategy, HUD, cover boxes, driving
  position, headlights. New **Traction control** and **Brake bias** settings (`[Shortcuts] TcUpKey/TcDownKey`,
  `BiasForwardKey/BiasRearwardKey`; brake bias shows the car's real front share). Settings whose panel is off are
  greyed out with "(panel off)". A line above each list names the wheel button that acts on it.
- **Rotary lists, knob box and messages on top** of every other panel, text included (panels' text used to show
  through them).
- **Fuel-saving bar**: the lines under it are now a clear table: stint target, stint left, how long the fuel lasts
  at last lap's use, and fuel per lap (average, and how far last lap was under or over). The target is set as soon
  as you leave the pit lane instead of a lap later.
- **Rotary lists and knob box** use the same dark grey as the other panels.
- **Tyre wear target** boxes narrower and without the empty space inside.
- **Track map**: the north arrow moved outside the box, so the track never runs through it. The **wet sections**
  colour mode is removed (it did not show the wet parts reliably).
- **Shift cue reworked**: you set the RPM you want to shift at with the new **Shift point** rotary setting (1% a
  click), and the cue learns, per gear, how early to flash so your actual shift lands there. Saved per car entry
  (`ShiftTargets\`), no longer shared by every car in a class; it starts from the old class value.
- **Icons** beside each rotary setting, in its group colour.
- **Shift cue** no longer shows in top gear. The **shift stats** box keeps its width at gear changes instead of
  jumping.
- **Rotary lists** moved to the sides of the screen (left knob left; right knob and on/off list right), smaller,
  10 lines with the sliding scroll back, fixed width, numbered and left-aligned. The knob box at the bottom is a fixed
  width with L at the far left and R at the far right.

### New
- **Update check**: at each game start the plugin asks GitHub (api.github.com) for the latest release and, if it is
  newer, says so in a message (once, in the garage, never while driving). Nothing is downloaded or
  installed. Turn it off with "Update check" in the on/off list (`[Updates] CheckForUpdates`).
- **Replay recording** in the frame-time stats box (off by default, `[FrameLatency] ShowReplay=1`): off / to RAM /
  to disk with the recording's size so far (amber from 1.5 GB, red from 1.9 GB), and the replay fidelity setting
  (Low / Medium / High / Max).
- **RAM and VRAM stats** reworded: "RAM 18.2 used, rF2 9.1, 13.8 free GB" and "VRAM 9.8 used, rF2 8.1, 6.2 free GB" (whole card, like RTSS).
- **Rotary menus**: an on/off list and two setting lists, on Home/End, Insert/Delete and Page Up/Page Down, or on
  wheel buttons set in `[Shortcuts]` (none set by default, since every wheel numbers its buttons differently; the
  guide explains how to find yours).
- **Gap box**: a small second table with the two cars ahead and two behind you in the race order, picked from the
  whole field (retired cars left out). Own position, size and on/off row in the shortcut list.
- **Your session best** (PB) in the standings footer, after the session's fastest lap.
- **HUD always on** (Ctrl+Shift+A): draws the HUD while you are not driving, e.g. riding along while a team-mate
  drives your shared car.
- **Track conditions and weather** box: slicks/wets call, racing-line wetness and rain trends, the game's own
  forecast, and an optional real-world forecast from Open-Meteo (off by default; the only part that uses the
  internet).
- **DLL cost** panel (off by default): per-panel frame cost of the plugin.
- Shortcut-list rows to switch the fuel save bar, tyre wear target, water/oil temps, FFB strength, track conditions
  and real weather on or off.
- **Stopped car warning**: a yellow line above the gear/speed boxes, "STOPPED 420 m", counting down and flashing
  inside 200 m (no sound). Stopped = under 40 km/h (in a hairpin you take that slowly yourself, also under 80% of
  your best-lap speed there). Cars within 150 m of each other are one incident, "STOPPED 420 m (3 cars)", and
  "OFF TRACK" is added when they are all beyond the track edge. Pit lane, finished cars, the start and full-course yellows are left out. Settings in `[TrackAlerts]`.
- **Yellow flag sector** ("YELLOW S2", needs local yellows on the server) and "FULL COURSE YELLOW" in the same line.
- **Pit lane limit per track**: a new knob setting, saved in `PitLaneLimits\`. With the limiter on and above it,
  the PIT box turns red and reads "OVER SPEED". Once a pit stop is requested, "PIT LIMIT 60 km/h" shows above the
  speed box until you are in the pit lane or the limiter is on (the learned limit with "~" if none is set).

### Changed
- **Pit-limiter reminder removed** (the "ENGAGE PIT LIMITER" warning and its beep, and the pit-entry learning
  behind it). The `PitEntryOffsets` folder is no longer used.
- **Pit limiter warning** is ignored while the AI is driving your car (rFactor 2 reports the limiter as on then).
- **DLL cost panel** header shows ms per frame (average and 99.9%) and % of a core.
- **Pit limiter**: the big PIT bar is gone. PIT flashes in the small ABS/TC box (green under the limit, red over,
  amber while the limit is not yet learned); gear and speed stay visible.
- **FFB spectrum**: up to 200 Hz (half the 400 Hz FFB rate), on a log frequency axis labelled at the Moza Pit House
  EQ points, drawn in log bands. New setting `[FfbGraph] BandsPerDecade` (default 5; 10 = 1/3-octave; 0 = every
  1 Hz).
- **Layout mode**: every panel, cover boxes included, resizes from any of its four corners, with the opposite corner
  held in place. The standings and gap box fill with placeholder rows so they are placed at their full size.
- **Brake bias**: shown as the front share (e.g. 56.4% Front), as rFactor 2's garage gives it; it showed the rear share before.
- **Shortcut list**: every panel and on/off option in one list, grouped (Driving, Timing, Fuel/tyres/brakes, Car & FFB, Track & weather, Screen). While open, every switched-on panel is shown at full size, with placeholder content where it has no data yet. The fuel save row is renamed "Fuel use delta bar (fuel save)".
- **Pedal trace**: lower CPU use: flat stretches of a pedal line (resting at 0% or held at 100%) are drawn as one
  segment. The lines no longer change shape as they scroll.
- **"Fuel + tyres" master switch removed**: the fuel, tyres and brake bias boxes each have only their own row.
- **Tyre wear plots removed** (row, drawing and their settings).
- **Shortcut list**: 30% smaller text, tighter rows, a dark, nearly solid background. While it is open, drag its
  title to move it and any corner square to resize it (no layout mode needed).
  Text size boxes (S / M / L) at the top of the list.
- **Riding along in a shared car**: the HUD follows your own car (the one you last drove, saved as
  `[Session] LastVehicle`) instead of mixing in every car's telemetry, which made gear, speed and the PIT warning
  jump around.
- **Standings**: the laps-since-pit column is hidden by default.
- **Stint helpers**: laps-based stint info, tyre life per corner, and a stint clock that counts only time on track.
- **Track map**: kept on the graphics card and only rebuilt when it changes, cutting its CPU cost on long tracks.
- **When files are saved**: the plugin now only writes to disk when you cross the line (only newly learned data:
  best lap, braking points, fuel and tyre profiles, pit limit, shift points, pit entry, recorded maps) or when you
  are out of the car (everything else: settings, layouts, the log), and always on a background thread. Leaving
  layout mode no longer saves on the spot: the new positions are written once you are out of the car. A game crash
  mid-stint loses only that lap's learning and the layout or setting changes made since you got in.
- **Pit stints** (laps since each car's last stop) are no longer saved to a file; after restarting the game a car
  shows "?" until it next pits.
- **Smoother frame times**: the hitches when leaving layout mode, at a beep, at a car or track change and when the
  track map's sector colours change are gone. Each frame also costs less: standings, radar, FFB spectrum, pedal
  trace and the rest of the drawing.
- **Track conditions** and **engine temps** boxes update five times a second.
- **GPU usage** in the stats line counts only rFactor 2's own GPU work, not other programs'.

### Fixed
- The frame-time stats box could overrun its memory with every extra stat switched on.
- Wheel buttons stayed dead for the whole session if the wheel was not found when the game started; the plugin now
  looks for it again every 2 s.
- A possible crash when the game resized or recreated its window.
- A possible crash when a setting changed at the moment the plugin was saving.
- Changing car could keep the previous car's per-gear shift points.

## 26.11.17.3
See the GitHub release.

## 26.11.17.2
- Braking-point beep (BrakeCue), off by default (`ShowBrakeCue=0`).

## 26.11.13.1
- First public release.
