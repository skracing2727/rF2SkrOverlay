# Changelog

## 26.11.25.3

### Added
- **Rotary lists can be moved and resized** in layout mode (both sides are shown while arranging).
- **Box rate + cost labels**: a shortcut-list option shows, in each box's corner, how many times a second it redraws and what it costs per frame in milliseconds.
- **Enter key** switches the highlighted item in the knob on/off list, the same as the on/off wheel button (`ElementToggleButton`), for using the knob menu from the keyboard.
- New **Benchmark box** (off by default, shortcut list -> Screen; `[Benchmark] Enabled`): FPS through each lap for present, CPU and GPU, with the last 10 laps kept, plus a live frame-time histogram (earlier laps faint behind). It warns when the GPU isn't the limit, because GPU time then includes idle time (`GpuBusyWarnPercent`, default 95).
- **Fuel per stop and required stops** (advanced fuel box): the fuel still needed is split evenly over the stops left, for about equal stints and the lightest car. `[FuelSave] RequiredStops` (0-9, or the **Required stops** knob setting) adds stops the race rules require; stops already made count against it.
- **Fuel box: simple and advanced modes** (`[FuelSave] FuelBoxMode`). Simple has no bar and needs no stint target: a
  small table of fuel and how long it lasts, at stint start and now. "Now" is red if the fuel runs out before the
  finish, amber if less than a lap is spare, green otherwise (white outside races). Advanced is the bar and its table.
- **Pit stops left** and **Fuel for last stop** rows in the advanced fuel table (litres to add at the final stop
  after the full tanks); they replace the short-lived "Fuel to finish" row.
- **Fuel box** knob setting (strategy group) to switch between simple and advanced.

- **Track alerts**: a blue **BLUE FLAG** line, a red **PENALTY** line (with a count if more than one; rFactor 2
  does not say which penalty), and full-course yellow shown by stage in amber (SC deployed, pits closed, pits open
  for the lead lap, pits open, last lap, green next lap, race halted).
- **Hint for unbound wheel buttons**: with no wheel buttons bound, the knob box and rotary lists say "Bind keys in the
  INI [Shortcuts] -- rotary encoders on your rig work best".

### Changed
- More boxes redraw below the frame rate (relative timing and gap box at 10 Hz, FFB strength, DRS, track alerts, DLL cost, cover boxes), boxes on the same rate no longer redraw on the same frame, and the fuel and tyre boxes' numbers update 5 times a second while their bars stay smooth.
- The overlay's own background threads (saving, lookups, polling) now run in Windows' efficiency mode, so on CPUs with E-cores they keep off the cores the game uses.
- Benchmark box: the percentile calculations run on a background thread in efficiency mode (E-cores) instead of the game's render thread.
- **Penalty warning**: the PENALTY line now shows for 5 seconds when a penalty arrives, then shrinks to a small red P (or P2, P3) tag on the gear box until it is served, instead of covering the road view.
- **Shortcut list easier to scan**: each group has its own colour (heading, a stripe at the left edge, the key text), each row has a small icon, and hovering a row shows a mini map of the screen with that panel's box highlighted.
- **Benchmark box** (new in this release): avg / 1% low / 0.1% low table inside the FPS graph; every frame's point stays for the lap with an outlined average line; axis breaks squeeze empty ranges; histogram in 0.01 ms steps (click it to switch to FPS in 0.1 FPS steps), marker lines up to 240 FPS, as % of frames, zoomed to the data (20 ms at most), with dotted 60/90/120/140/144 FPS lines and fading per-frame dots; FPS graph scales itself; lap averages in the legend; dots fade in 1 s; the GPU warning only shows when the GPU is not the limit.
- Guide and README now credit both open-source projects whose code was consulted: TinyPedal and CrewChief.
- HUD layouts: when the player profile can't say which car you're in, the layout now follows the car name from rF2's web interface instead of the car class, so cars in the same class no longer share a layout.
- **Radar and side bars** now use every car's live telemetry (about 50 times a second) instead of the slower timing
  feed, so other cars no longer lag about 0.2 s behind where they really are.
- **Saved data is per car model, not per class**: best lap, braking points, fuel figures, fuel and tyre profiles, pit
  limit and shift points. Two different GT3 cars no longer share one PB. A car with no file yet starts from your
  old class file, which is kept. The car model is read from rFactor 2's own web interface on your PC (port 5397);
  if that is off, the car's entry name is used.
- **Track alerts and PIT LIMIT** show even with the gear/speed panel hidden.
- **OVER SPEED** also shows in the pit lane when the pit limiter is off.
- **DRS sign** is its own panel: it shows with the delta bar off, has its own position, and its size follows only
  `[LapDelta] DrsScale` (no longer multiplied by the delta bar's Scale).
- **Pedal trace**: the after-TC / after-ABS lines are drawn faint in the same colour under your own pedal lines, so
  the electronics cutting in shows as a shadow.
- **Track map**: a saved or TinyPedal map whose length does not match the track (more than 2% off) is stretched to
  fit while a new map is recorded from your next clean lap.
- **Tyre wear target**: the set clock and lap count keep running while the panel is hidden.
- Removed the unused `[TrackMap] DeltaFullSeconds` setting and the braking-point beep's unused brake-trace
  recording (old `.brake` files are ignored).
- **Fuel-saving bar table**: "Stint target" also shows the litres per lap that stint allows, and the row
  **Fuel per lap target** shows that target and last lap's difference to it (minus = under, green; plus = over, red).
- **Gap box**: in practice and qualifying it shows the best-lap difference, in races the race interval (it used to
  show the on-track gap). Session-fastest laps show in purple.
- **Pedal trace**: the brake "after ABS" line now uses the wheels' brake pressure, so ABS cutting in shows as a gap.
- **Wheelspin** works out the driven axle per car, so front-drive and four-wheel-drive cars get the spin strip and
  speed-box tint too. `[PedalTrace] SpinAxle`, `SpinDeadzone` and `SpinFullScale` are now read from the INI.

### Fixed
- **GPU busy figure** (Benchmark box, DLL cost): the Windows GPU counter path was malformed, so GPU busy could not be read.
- **Fuel box**: before your first clean lap, a lap that included time in the pits or garage could be used as the lap time, so 'Fuel lasts', the stint laps and the pit-stop rows showed absurd numbers while stopped. Such laps are now ignored.
- Safety-car / full-course-yellow laps no longer lower the learned fuel per lap.
- Timed races: laps left (and so fuel needed) now count at your fastest clean lap, so the estimate errs a lap high, not short.
- The standings no longer freeze when the leader finishes; they freeze when you do.
- The braking-point beep now also works on your last lap after the leader finishes and under full-course yellow.
- A lap whose time rF2 voided (e.g. a track-limits cut) is no longer saved as your personal best.
- Sector yellows now show in the right sector.
- Full-course-yellow line: 'PENDING' and 'SC OUT' instead of 'SC DEPLOYED' and 'PITS CLOSED'; the resume stage no longer shows 'GREEN NEXT LAP' (it is already green).
- No false 'STOPPED' alerts from cars with no data or in the first 30 s of a session; cars not yet placed on track are left out of the radar.
- Disconnected online cars are no longer listed in the standings.
- The garage counts as the pits for fuel and stint tracking.
- Radar side bars: no flicker at the edge of the window, a short delay before clearing, none in the pit lane, and none for a car passing much faster or slower (`[Radar] SideBarMaxClosingSpeedMps`, default 12).
- **Accented letters** in driver and track names (Perez, Nurburgring) now draw as plain letters instead of vanishing.
- **File names** from track or car names in non-Latin scripts get a short code, so two such tracks no longer share
  one file. Ordinary names are unchanged.
- **Fuel to finish in timed races** (fuel bar and Fuel panel) could be up to a lap short: a timed race ends at the
  end of the lap you are on when the clock runs out, and the estimate now counts to that line. Both lap and timed
  races also count from where you are in the current lap.
- **Learning only from your own driving**: best laps, braking points, fuel use, shift points, pit limit and tyre
  wear ignore laps where the AI or a team-mate drove your car, and laps with a pit, garage or teleport moment.
- **Fuel**: lap estimates use a clean-lap pace (a lap with a stop no longer makes the figures optimistic); fuel use
  learns again in wet races and slower cars; timed races keep their figures after 0:00; the fuel table stays in the
  pit lane; a drive-through or a stop without fuel no longer restarts the stint; no green dip after each line.
- **Braking-point beep**: the braking points were placed slightly wrong (fixed); beeps fire for every point in any
  order; online "count lap but not time" laps count; no beeps in the pit lane, under yellow or safety car.
- **Radar and side bars**: cars in the garage are hidden, pit-lane cars are faint and never red; no bar for a car
  directly behind, oncoming or crossing; a car reset by the game jumps instead of gliding past you.
- **Lap delta and mini sectors**: the line hold shows the real lap result; the trend colour no longer flashes; the
  last mini sector is filled; the strip sits right on non-16:9 screens.
- **Shift cue and gear**: the cue shows in 1st gear and reacts at once; OVER SPEED really flashes faster than PIT;
  pit limit learned more reliably; the speed box no longer changes width with the digits.
- **Standings**: accented names keep their letters; lap times just under a minute no longer print as "1:60.0";
  in/out laps no longer throw the gaps off; big fields (over 64 cars) and retired cars handled.
- **Track map**: session fastest and purple work on more servers; cut laps are not recorded as the map; TinyPedal
  maps found for accented track names; retired and garage cars hidden, pit cars dimmed.
- **Tyre wear target**: only clean laps count, "Over max" shows in red, a fronts-only change keeps the rears' history.
- **Weather**: the real forecast shows rain for the right hour and no longer shows the previous track's forecast;
  engine temperatures turn amber on slow overheating.
- **Rotary knobs**: no key presses sent while you are out of the car or rFactor 2 is not in front (no more keys typed
  into Discord); "+N steps" counts only presses actually sent.
- **Frame-time stats**: CPU time and FPS read correctly (were 5-15% off); GPU time works on non-English Windows.
- **Stability**: several possible crashes and freezes fixed (long text, caught panel faults, quitting during the
  update check, failed INI saves now warned about). Hand-edited INI values read more safely ("0,8" works). The
  track-map colour buttons are no longer bound by default.

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
