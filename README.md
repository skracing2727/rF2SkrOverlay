# rF2SkrOverlay

A free in-car HUD for rFactor 2, drawn straight into the game: no extra windows, nothing to alt-tab to.

- Radar with side proximity bars
- Gear, speed and shift light, with ABS/TC and tyre lockup indicators
- Pedal trace
- Lap delta bar and mini-sector strip
- Relative timing
- Fuel, tyre wear and brake bias
- Tyre temperatures, track map and FFB spectrum

Every panel can be dragged, resized and switched on or off in game (Ctrl+Shift+2 for layout mode), and the
layout is remembered for each car. Works on Linux/Proton too, with a built-in font.

## Download and install

Get the latest zip from [Releases](../../releases), then drop `rF2SkrOverlayPublic.dll` into
`rFactor 2\Bin64\Plugins`. The full guide, `rF2SkrOverlayPublic_Guide.html`, is in the zip and in this repository.

## Building

Run `build.bat` (needs the Visual Studio C++ build tools; see the guide's licence section).

## Licence

GPL-3, see [LICENSE.html](LICENSE.html). The built-in DejaVu Sans Mono font is under its own free licence, see
[FONT-LICENSE.html](FONT-LICENSE.html). `InternalsPlugin.hpp` and `PluginObjects.hpp` are Image Space
Incorporated's rFactor 2 plugin SDK headers and remain under ISI's terms.

Thanks to [TinyPedal](https://github.com/TinyPedal/TinyPedal) for help with the code, and to the SRGP community for
testing.

By Paul Kiernan, Shamrock Kiernan Racing (SKR).
