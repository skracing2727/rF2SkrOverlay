#include "Config.h"
#include "Log.h"
#include "Renderer.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

TextBackgroundConfig g_textBackgroundConfig;
TextFontConfig g_textFontConfig;
ClockConfig g_clockConfig;
DllCostConfig g_dllCostConfig;
PerfUpdateConfig g_perfUpdateConfig;
ShortcutConfig g_shortcutConfig;
RadarConfig g_radarConfig;
PedalConfig g_pedalConfig;
MiniSectorConfig g_miniSectorConfig;
TyreTempsConfig g_tyreTempsConfig;
FfbGraphConfig g_ffbGraphConfig;
TrackMapConfig g_trackMapConfig;
CoverBoxConfig g_coverBoxConfig;
LayoutConfig g_layoutConfig;
FrameLatencyConfig g_frameLatencyConfig;
GearDisplayConfig g_gearDisplayConfig;
PitLimiterConfig g_pitLimiterConfig;
LapDeltaConfig g_lapDeltaConfig;
RelativeTimingConfig g_relativeTimingConfig;
FuelTyrePanelConfig g_fuelTyrePanelConfig;
FuelSaveConfig g_fuelSaveConfig;
TyreSaveConfig g_tyreSaveConfig;
EngineTempsConfig g_engineTempsConfig;
FfbLevelConfig g_ffbLevelConfig;
BrakeCueConfig g_brakeCueConfig;
DrsCueConfig g_drsCueConfig;

namespace
{
    char s_iniPath[MAX_PATH] = {};

    // Written verbatim if the INI doesn't exist yet, so there's a real,
    // commented starting point covering every editable value -- nothing here
    // needs a rebuild to change.
    const char* kDefaultIni =
"; rF2SkrOverlayPublic settings.\n"
"; Edit values and restart rFactor 2 (or re-tick the plugin in\n"
"; Settings -> Gameplay -> Plugins) to apply -- no rebuild needed.\n"
"; Fractions are 0..1 relative to screen width/height, so the layout holds at\n"
"; any resolution. Colors are 0..1 per channel.\n"
"\n"
"[Radar]\n"
"; 1 = show the radar, 0 = hide it completely (nothing drawn, no cost).\n"
"Enabled=1\n"
"\n"
"; Radar width as a fraction of screen width.\n"
"WidthFraction=0.166667\n"
"; Where your car (radar centre) sits on screen. 0.5,0.5 = dead centre.\n"
"CenterXFraction=0.5\n"
"CenterYFraction=0.333333\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"Scale=1.0\n"
"\n"
"; Detection range in each direction, in meters. Cars fade in smoothly as the\n"
"; boundary is approached from outside, not modelled as a hard circle -- it's\n"
"; an ellipse using these three values.\n"
"RangeForwardMeters=10.0\n"
"RangeRearMeters=10.0\n"
"RangeSideMeters=20.0\n"
"; Where the edge fade begins, as a fraction of the way to the boundary (0..1).\n"
"FadeStartFraction=0.7\n"
"\n"
"; Car footprint. Real GT car is about 2.0m wide, 4.6m long -- these are HALF\n"
"; those values.\n"
"CarHalfWidthMeters=1.0\n"
"CarHalfLengthMeters=2.3\n"
"CarCornerRadiusMeters=0.45\n"
"; 1 = draw a car at its real class footprint (GT3/GT4/TCR-touring/prototype/\n"
"; open-wheel, matched from the car's own class/name) instead of the flat box\n"
"; above -- that box remains the fallback for any car matching no known\n"
"; category, and is used for everyone when this is 0.\n"
"UseRealCarSize=1\n"
"; Added to every car's half-width AND half-length after the lookup above --\n"
"; a flat safety margin on top of whatever size a car is drawn at. 0 = off.\n"
"FootprintBufferMeters=0.0\n"
"\n"
"PlayerColorR=0.15\n"
"PlayerColorG=1.0\n"
"PlayerColorB=0.15\n"
"; Clear ahead/behind, no immediate danger.\n"
"OtherColorR=1.0\n"
"OtherColorG=0.85\n"
"OtherColorB=0.1\n"
"; Red when a car is genuinely beside you: inside BOTH the longitudinal and\n"
"; the lateral window below, in raw centre-to-centre metres. The exact same\n"
"; test drives the on-screen side indicator, so the two can never disagree\n"
"; about whether a car counts. DangerLateralMeters also sets where the side\n"
"; indicator is fully green, ramping to red as the car closes from there.\n"
"DangerColorR=1.0\n"
"DangerColorG=0.15\n"
"DangerColorB=0.15\n"
"DangerAheadMeters=4.0\n"
"DangerBehindMeters=5.0\n"
"DangerLateralMeters=10.0\n"
"\n"
"; A brief, very translucent yellow wash pulsing over the whole radar\n"
"; whenever any tracked car is in the red danger zone above -- a\n"
"; peripheral-vision cue, never opaque enough to obscure the car blips.\n"
"DangerFlashEnabled=0\n"
"DangerFlashColorR=1.0\n"
"DangerFlashColorG=0.9\n"
"DangerFlashColorB=0.1\n"
"DangerFlashMaxAlpha=0.18\n"
"DangerFlashPeriodSeconds=0.5\n"
"\n"
"; --- On-screen side indicator (the red bars down the screen edges) ---\n"
"; WHICH cars trigger a bar is not set here: that is the Danger*Meters\n"
"; window above, shared with the radar's red blips so the two can never\n"
"; disagree. Everything below is purely how a bar looks once it is showing.\n"
"\n"
"; Maximum width of one bar, as a fraction of screen width -- the width at\n"
"; the end nearest the car; the far end tapers in.\n"
"SideBarWidthFraction=0.35\n"
"; Live-adjustable from the resize handle and opacity slider that appear\n"
"; next to the bar in layout mode (Ctrl+Shift+2) -- dragging them writes\n"
"; straight back here, same as everything else.\n"
"SideBarScale=1.0\n"
"SideBarOpacityMultiplier=1.0\n"
"\n"
"; One flat colour throughout: closeness is carried by opacity and shape,\n"
"; never hue.\n"
"SideBarColorR=1.0\n"
"SideBarColorG=0.0\n"
"SideBarColorB=0.0\n"
"\n"
"; Lateral distance drives base opacity: full strength at or inside\n"
"; SideBarNearMeters, falling to the far value at DangerLateralMeters where\n"
"; the bar first appears. These are the alpha at the SCREEN EDGE -- with the\n"
"; inner-edge fade below, a bar reads as roughly half this across its width.\n"
"SideBarNearMeters=3.0\n"
"SideBarAlphaNear=0.60\n"
"SideBarAlphaFar=0.18\n"
"\n"
"; Alpha multiplier at the bar's inner edge. 0 dissolves it completely;\n"
"; above 0 leaves a hard vertical line across the view.\n"
"SideBarInnerEdgeAlpha=0.0\n"
"\n"
"; Forward offset leans the bar toward the end the car is at: ahead weights\n"
"; the top, behind the bottom, level leaves it even. EndFade is how far the\n"
"; far end's opacity drops at the extreme of the danger window (1 = to\n"
"; nothing), EndTaper how far it narrows, and Gamma sharpens the response to\n"
"; SMALL offsets -- below 1 a modest offset already gives an obvious lean.\n"
"SideBarVerticalEndFade=1.0\n"
"SideBarVerticalFadeGamma=0.5\n"
"SideBarWidthEndTaper=0.75\n"
"\n"
"; Asymmetric on purpose: a safety-buffer warning must not be delayed on the\n"
"; way in, but a slow release stops the bar strobing when a car hovers right\n"
"; on the activation boundary.\n"
"SideBarFadeInSeconds=0.10\n"
"SideBarFadeOutSeconds=0.35\n"
"; Fade-to-green: as a car's own contribution fades out (it has left the\n"
"; danger window), the bar's colour shifts continuously toward this one\n"
"; instead of just dimming to nothing.\n"
"SideBarFadeToGreenEnabled=1\n"
"SideBarFadeToGreenColorR=0.15\n"
"SideBarFadeToGreenColorG=1.0\n"
"SideBarFadeToGreenColorB=0.15\n"
"; \"Too close\" flash: independent of the rule that decides whether the bar\n"
"; shows at all -- once a car IS triggering it, if it is also this close\n"
"; laterally (meters), that car's own contribution blinks between the bar's\n"
"; normal colour and this one instead of sitting solid.\n"
"SideBarTooCloseFlashEnabled=1\n"
"SideBarTooCloseMeters=3.0\n"
"SideBarTooCloseMetersMax=5.0\n"
"SideBarTooCloseFlashHz=4.0\n"
"SideBarTooCloseFlashHzMax=10.0\n"
"SideBarTooCloseFlashColorR=1.0\n"
"SideBarTooCloseFlashColorG=0.85\n"
"SideBarTooCloseFlashColorB=0.0\n"
"\n"
"[PedalTrace]\n"
"; 1 = show the pedal trace, 0 = hide it completely (nothing drawn, no cost).\n"
"Enabled=1\n"
"\n"
"WidthFraction=0.3\n"
"HeightFraction=0.08\n"
"; Where the panel's centre sits, same convention as Radar above.\n"
"CenterXFraction=0.5\n"
"CenterYFraction=0.9516\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"Scale=1.0\n"
"\n"
"BackgroundR=0.08\n"
"BackgroundG=0.08\n"
"BackgroundB=0.08\n"
"BackgroundA=0.6\n"
"BackgroundCornerRadiusPixels=10.0\n"
"\n"
"; Thin bezel outline, matching the gear/speed dashboard's.\n"
"BorderEnabled=1\n"
"BorderThicknessPixels=2.0\n"
"BorderColorR=0.5\n"
"BorderColorG=0.5\n"
"BorderColorB=0.5\n"
"\n"
"; Two lines per pedal: Raw = what you pressed, Actual = what the car got\n"
"; after TC (throttle) or ABS (brake). The gap between them is the\n"
"; electronics intervening.\n"
"ThrottleRawR=0.15\n"
"ThrottleRawG=0.85\n"
"ThrottleRawB=0.95\n"
"ThrottleActualR=0.05\n"
"ThrottleActualG=0.45\n"
"ThrottleActualB=0.55\n"
"BrakeRawR=1.00\n"
"BrakeRawG=0.70\n"
"BrakeRawB=0.00\n"
"BrakeActualR=0.60\n"
"BrakeActualG=0.40\n"
"BrakeActualB=0.00\n"
"\n"
"; Matched to their associated pedal: lockup happens under braking, spin\n"
"; happens under throttle.\n"
"LockupR=1.00\n"
"LockupG=0.70\n"
"LockupB=0.00\n"
"SpinR=0.15\n"
"SpinG=0.85\n"
"SpinB=0.95\n"
"\n"
"; Wheel slip sensitivity -- fraction difference between wheel surface speed\n"
"; and the ground speed under that wheel.\n"
"; Below SlipDeadzone: normal driving, no indicator. At/above SlipFullScale:\n"
"; indicator shown at full intensity. Below MinSpeedMetersPerSec: ignored\n"
"; entirely (slip is meaningless near-stationary).\n"
"SlipDeadzone=0.06\n"
"SlipFullScale=0.35\n"
"MinSpeedMetersPerSec=2.0\n"
"; Multiplies the lockup/spin magnitude before display -- both the VU-meter\n"
"; bar height and its colour intensity scale with it, so the indicators reach\n"
"; full size/brightness from a smaller amount of actual slip.\n"
"SlipIntensityMultiplier=6.0\n"
"\n"
"; The whole panel fades out after being both stationary AND off the pedals\n"
"; for this many seconds (requiring both avoids fading during a normal\n"
"; lift-and-coast mid-corner), and fades back in over the same duration.\n"
"IdleFadeSeconds=2.0\n"
"\n"
"[BrakeCue]\n"
"; 1 = beep at your best lap's braking points, 0 = disabled entirely.\n"
"Enabled=1\n"
"\n"
"; Brake input above this counts as \"braking\" for recording a point --\n"
"; deliberately high so only real braking points get recorded, not a light dab.\n"
"BrakeThreshold=0.2\n"
"; Ignore a new point this close to the last one (0..1 brake input treated as\n"
"; the same zone, e.g. pumping the brakes rather than a separate corner).\n"
"MinPointSpacingMeters=15.0\n"
"; Only beep for big stops: a braking point whose speed drop (on the reference\n"
"; lap, from where braking began down to the slowest point before the next\n"
"; braking point) is less than this many km/h stays silent. 0 = beep for all.\n"
"; Also a slider in the shortcut list.\n"
"MinSpeedDropKmh=50\n"
"; Lead time: the beep fires this many SECONDS before the recorded point,\n"
"; converted to a distance using your current speed -- so the warning is the\n"
"; same amount of TIME whether you're approaching at 250 km/h or 90 km/h.\n"
"; With AdaptiveLeadTimeEnabled=1 (below) this is only the starting value.\n"
"LeadTimeSeconds=0.3\n"
"\n"
"BeepFrequencyHz=880\n"
"BeepDurationMs=120\n"
"\n"
"; --- Reaction-time measurement and adaptive lead time ---\n"
"; The gap between each beep and the brake pedal actually moving is timed,\n"
"; and the beep's lead time is then set to your own rolling average -- so\n"
"; the pedal moves AT the reference lap's braking point instead of a fixed\n"
"; guess early or late. React slowly and the beep moves earlier; react\n"
"; quickly and it moves later. Off by default: braking later than the\n"
"; reference on purpose looks like a slow reaction, so the beep keeps moving\n"
"; earlier. 0 = a fixed LeadTimeSeconds.\n"
"AdaptiveLeadTimeEnabled=0\n"
"; Brake input above this counts as \"the pedal has moved\" for that timing --\n"
"; far lower than BrakeThreshold above, since this wants the first instant of\n"
"; movement, not a committed braking point.\n"
"ReactionThreshold=0.10\n"
"; A beep with no pedal movement within this long is abandoned rather than\n"
"; matched against whatever braking eventually follows (a missed corner, a\n"
"; spin, or simply ignoring the cue).\n"
"ReactionTimeoutSeconds=3.0\n"
"; Individual measurements outside this range are discarded instead of\n"
"; dragging the average around (a real reaction to a sound while driving is\n"
"; ~0.15-0.6s). The ceiling also bounds how early the cue can ever be moved.\n"
"ReactionMinSeconds=0.02\n"
"ReactionMaxSeconds=1.5\n"
"\n"
"; On-screen readout: average reaction, most recent one, and the lead time\n"
"; currently in use. Hidden until the first reaction has been measured.\n"
"ShowReactionStats=1\n"
"ReactionStatsCenterXFraction=0.5\n"
"ReactionStatsCenterYFraction=0.982\n"
"ReactionStatsTextHeightPixels=28.0\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"ReactionStatsScale=1.0\n"
"ReactionStatsColorR=1.0\n"
"ReactionStatsColorG=1.0\n"
"ReactionStatsColorB=1.0\n"
"ReactionStatsBackgroundEnabled=1\n"
"ReactionStatsBackgroundPaddingPixels=8.0\n"
"ReactionStatsBackgroundCornerRadiusPixels=8.0\n"
"ReactionStatsBackgroundR=0.08\n"
"ReactionStatsBackgroundG=0.08\n"
"ReactionStatsBackgroundB=0.08\n"
"ReactionStatsBackgroundA=0.6\n"
"\n"
"[MiniSector]\n"
"; Vertical position of the delta-speed strip. -1 means stacked directly\n"
"; above the lap-delta bar, which is where it lived when it had no Y of\n"
"; its own -- dragging the delta bar moved it too. The first drag\n"
"; resolves this and the strip becomes an independent box.\n"
"CenterYFraction=-1\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"Scale=1.0\n"
"; Two lap-long strips above the delta bar: the previous completed lap on\n"
"; top, the lap in progress below it, filling in left-to-right as you drive.\n"
"; Each mini-sector is coloured by how it compared to the reference lap --\n"
"; green quicker, red slower -- so you can see WHERE the lap time went, not\n"
"; just the running total. The per-segment deltas add up to the delta bar's\n"
"; own number.\n"
"Enabled=1\n"
"\n"
"; Mini-sectors per lap. This is a COUNT, not a length: the strip always\n"
"; spans exactly one lap whatever the track, so each segment is\n"
"; (track length / count) metres. Binning by distance rather than time is\n"
"; deliberate -- it makes each pixel column the same stretch of track no\n"
"; matter how fast it was taken.\n"
"SegmentCount=200\n"
"\n"
"; Position and size both follow the delta bar (see [LapDelta]) so the three\n"
"; read as one stack: these sit directly above it, and by default take their\n"
"; width from its BarWidthFraction. Set MatchDeltaBarWidth=0 to size them\n"
"; independently with WidthFraction below instead.\n"
"MatchDeltaBarWidth=1\n"
"WidthFraction=0.3333\n"
"CenterXFraction=0.5\n"
"BarHeightPixels=54.0\n"
"RowGapPixels=6.0\n"
"GapAboveDeltaBarPixels=10.0\n"
"\n"
"; 1 = compare against this session's best lap, 0 = against the all-time\n"
"; best saved to disk. Same two references the delta bar itself shows.\n"
"UseSessionReference=1\n"
"\n"
"; Colour scale: a fixed LADDER of rungs (ScaleStepKmh, 2x, 3x, ... up to\n"
"; ScaleStepCount of them), not a value that fits smoothly to the data --\n"
"; full green/red sits at +/- the SMALLEST rung that still covers\n"
"; ScalePercentile% of the speed differences currently on screen (both\n"
"; strips combined). Most laps sit on the tightest rung; one genuinely bad\n"
"; segment jumps the WHOLE scale up a rung instead of just clipping\n"
"; unremarked. Recomputed every frame, so the previous lap's strip is\n"
"; always shown under the CURRENT rung, not whatever was live when it was\n"
"; the current lap.\n"
"ScaleStepKmh=5.0\n"
"ScaleStepCount=6\n"
"ScalePercentile=95.0\n"
"\n"
"; Small \"+X / -X km/h\" readout beside the strips showing the rung\n"
"; currently in use -- needed since it is no longer a fixed number.\n"
"ShowScaleLabels=1\n"
"ScaleLabelTextHeightPixels=20.0\n"
"ScaleLabelGapPixels=10.0\n"
"\n"
"; Also the \"+X\"/\"-X\" scale label colours above.\n"
"FasterColorR=0.15\n"
"FasterColorG=1.0\n"
"FasterColorB=0.15\n"
"SlowerColorR=1.0\n"
"SlowerColorG=0.15\n"
"SlowerColorB=0.15\n"
"; TEST scale: red -> intermediate (blue) -> green, instead of the plain\n"
"; red/green lerp above. Off by default.\n"
"UseTriColorScale=0\n"
"TriMidColorR=0.15\n"
"TriMidColorG=0.15\n"
"TriMidColorB=1.0\n"
"; Shown where you matched the reference -- the colour both sides fade to.\n"
"NeutralColorR=0.55\n"
"NeutralColorG=0.55\n"
"NeutralColorB=0.55\n"
"; Two extra stops between neutral and each full-saturation end, so a\n"
"; moderate gain/loss reads as a visibly different shade than a small one\n"
"; -- a flat two-colour scale made anything past a small deficit look the\n"
"; same shade of red or green.\n"
"MidFasterColorR=0.65\n"
"MidFasterColorG=0.80\n"
"MidFasterColorB=0.15\n"
"MidSlowerColorR=0.95\n"
"MidSlowerColorG=0.55\n"
"MidSlowerColorB=0.10\n"
"; A segment within this many km/h of zero deficit is drawn neutral\n"
"; instead of tinting red or green -- otherwise measurement noise near\n"
"; even pace flickers the strip for no real reason.\n"
"NeutralDeadbandKmh=0.8\n"
"Alpha=0.95\n"
"\n"
"; A thin line along each strip's own bottom edge: GREEN where that\n"
"; mini-sector was driven under acceleration, RED where the brake was used\n"
"; (above AccelLineBrakeThreshold, below). Replaces the old\n"
"; single tick that marked only where braking STARTED -- the line shows the\n"
"; whole braking zone and its length, which is what is worth comparing lap\n"
"; to lap. Sized to sit inside the gap below the strip (RowGapPixels /\n"
"; GapAboveDeltaBarPixels above) rather than growing the widget -- keep the\n"
"; height under the smaller of those two if you resize either.\n"
"AccelLineEnabled=1\n"
"AccelLineHeightPixels=5.0\n"
"; The accel/brake line under the strip tests EACH pedal against its own\n"
"; threshold. It used to be a single braking-or-not flag, so the\n"
"; accelerating colour appeared the instant you came off the brakes --\n"
"; right through the coasting and turn-in phase where neither pedal is\n"
"; doing anything. Segments with neither pedal now draw no line at all.\n"
"AccelLineBrakeThreshold=0.10\n"
"AccelLineThrottleThreshold=0.10\n"
"AccelLineAccelColorR=0.15\n"
"AccelLineAccelColorG=0.85\n"
"AccelLineAccelColorB=0.95\n"
"AccelLineBrakeColorR=1.00\n"
"AccelLineBrakeColorG=0.70\n"
"AccelLineBrakeColorB=0.00\n"
"AccelLineAlpha=1.0\n"
"\n"
"BackgroundEnabled=1\n"
"BackgroundPaddingPixels=6.0\n"
"BackgroundCornerRadiusPixels=8.0\n"
"BackgroundR=0.08\n"
"BackgroundG=0.08\n"
"BackgroundB=0.08\n"
"BackgroundA=0.6\n"
"\n"
"[FrameLatency]\n"
"; Real call-to-display latency, measured via DXGI's own frame statistics --\n"
"; not the same measurement method CapFrameX/PresentMon use (they trace\n"
"; lower-level ETW events), so the two numbers may not match exactly, but\n"
"; this is a genuine driver-reported figure.\n"
"Enabled=1\n"
"\n"
"TextHeightPixels=26.208\n"
"\n"
"; FPS, Display Latency, and Input Lag are all joined into one horizontal\n"
"; line (\"stat | stat | stat\"). CpuGpuPosXFraction is the box's horizontal\n"
"; centre; CpuGpuPosYFraction is its own outer BOTTOM edge (padding\n"
"; included, not a text anchor point) -- a small fraction places it just\n"
"; above that point, e.g. above the in-game rearview mirror.\n"
"CpuGpuPosXFraction=0.5\n"
"CpuGpuPosYFraction=0.05\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"Scale=1.0\n"
"; When 1 (checked first, overrides both position fractions above), the box\n"
"; sits flush against the screen's own bottom-right corner instead.\n"
"AlignBottomRightCorner=0\n"
"; When 1 (checked second), the box sits flush against the TOP of the\n"
"; screen instead, horizontally centred at CpuGpuPosXFraction.\n"
"AlignTop=1\n"
"AlignRightToPedalTrace=0\n"
"\n"
"ColorR=1.0\n"
"ColorG=1.0\n"
"ColorB=1.0\n"
"\n"
"; Smoothed frames-per-second, from the real Present-to-Present interval.\n"
"ShowFps=1\n"
"\n"
"; Rough total input-to-photon latency estimate (half the average physics\n"
"; tick period + Display Latency) -- a LOWER BOUND, since it can't see the\n"
"; physical controller's own USB/HID polling latency.\n"
"ShowInputLagEstimate=1\n"
"\n"
"; Extra diagnostic stats, off by default. Turn on whichever you want to see.\n"
"ShowCpuFrameTime=0\n"
"ShowProcessRam=0\n"
"ShowVram=0\n"
"ShowFpsRange=0\n"
"ShowGpuFrameTime=0\n"
"ShowGpuPower=0\n"
"\n"
"; Dark grey panel behind the line.\n"
"BackgroundEnabled=1\n"
"BackgroundPaddingPixels=1.0\n"
"BackgroundCornerRadiusPixels=8.0\n"
"BackgroundR=0.08\n"
"BackgroundG=0.08\n"
"BackgroundB=0.08\n"
"BackgroundA=0.6\n"
"\n"
"[GearDisplay]\n"
"Enabled=1\n"
"\n"
"; Default sits just above the pedal trace's default position, same\n"
"; horizontal centre.\n"
"CenterXFraction=0.5\n"
"CenterYFraction=0.83\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"Scale=1.0\n"
"\n"
"; The SPEED box background reports what the tyres are doing under\n"
"; braking and power: amber under locking, cyan under wheelspin. Locking\n"
"; is judged on the WORST wheel -- any one wheel locking matters even if\n"
"; the axle average looks fine.\n"
"SlipFeedbackEnabled=1\n"
"LockSlipThreshold=0.20\n"
"SpinSlipThreshold=0.25\n"
"; Nothing shows unless the brake is pressed at least this hard --\n"
"; otherwise engine braking would tint the box green on every entry.\n"
"SlipFeedbackBrakeInput=0.20\n"
"; A state must last this long to show, and holds at least this long once\n"
"; it does -- so noise cannot flash the box and a real brief lock cannot\n"
"; vanish in a single frame.\n"
"SlipFeedbackDwellSeconds=0.06\n"
"SlipFeedbackHoldSeconds=0.25\n"
"LockSlipColorR=1.00\n"
"LockSlipColorG=0.70\n"
"LockSlipColorB=0.00\n"
"SpinSlipColorR=0.15\n"
"SpinSlipColorG=0.85\n"
"SpinSlipColorB=0.95\n"
"\n"
"; Small ABS/TC box to the LEFT of the gear digit, drawn only while the\n"
"; electronics are actually intervening and naming which. Rides with the\n"
"; gear panel; not separately positionable.\n"
"AbsTcBoxEnabled=1\n"
"AbsTcBoxGapPixels=14.0\n"
"; Four-corner tyre lockup box: 2x2 grid (FL/FR front, RL/RR rear) next to\n"
"; the speed panel, each quadrant lit BY INTENSITY the moment that wheel is\n"
"; locking under braking -- reuses LockSlipThreshold above as the cutoff,\n"
"; TyreLockupFullScale as where it reaches full brightness. Fades out over\n"
"; TyreLockupFadeSeconds once a corner stops locking, rather than vanishing\n"
"; instantly.\n"
"TyreLockupBoxEnabled=1\n"
"TyreLockupPosXFraction=-1\n"
"TyreLockupPosYFraction=-1\n"
"TyreLockupScale=1.0\n"
"TyreLockupFullScale=0.6\n"
"TyreLockupFadeSeconds=5.0\n"
"TyreLockupColorR=1.00\n"
"TyreLockupColorG=0.70\n"
"TyreLockupColorB=0.00\n"
"GearTextHeightPixels=165.0\n"
"\n"
"NormalColorR=1.0\n"
"NormalColorG=1.0\n"
"NormalColorB=1.0\n"
"\n"
"; No telemetry field gives an explicit \"optimal shift RPM\" -- this is a\n"
"; fraction of the engine's actual rev limit instead. Flashes between the\n"
"; shift colour and the normal colour once RPM crosses it.\n"
"ShiftRpmFraction=0.93\n"
"ShiftColorR=1.0\n"
"ShiftColorG=0.15\n"
"ShiftColorB=0.15\n"
"\n"
"; TARGET shift point, as a fraction of the rev limit -- the RPM the blue cue\n"
"; is trying to get you to actually shift AT (the cue itself fires earlier,\n"
"; by your measured reaction time). Seed/fallback only: once upshifts have\n"
"; been observed the learned per-gear value takes over. Takes priority over\n"
"; the red shift-soon flash.\n"
"RevLimiterRpmFraction=0.96\n"
"RevLimiterColorR=0.25\n"
"RevLimiterColorG=0.5\n"
"RevLimiterColorB=1.0\n"
"\n"
"; THIRD state above the blue cue: genuinely about to hit the physical rev\n"
"; limiter, checked directly against max RPM rather than the learned target\n"
"; so it can't drift away with it. Takes priority over blue and red.\n"
"TrueLimiterWarnFraction=0.995\n"
"TrueLimiterColorR=0.6\n"
"TrueLimiterColorG=0.0\n"
"TrueLimiterColorB=0.8\n"
"\n"
"; How far ahead of the target RPM (seconds of RPM-climb) the blue cue\n"
"; fires, so YOUR reaction time is spent before the target instead of after\n"
"; it. Learned from your own upshifts once ShiftReactionAdaptiveEnabled=1\n"
"; and enough samples exist; the seconds value below is the seed/fallback.\n"
"ShiftReactionAdaptiveEnabled=1\n"
"ShiftReactionDefaultSeconds=0.15\n"
"ShiftReactionMinSeconds=0.02\n"
"ShiftReactionMaxSeconds=0.6\n"
"ShiftReactionTimeoutSeconds=2.0\n"
"; The cue only means anything while you are trying to accelerate -- gated on\n"
"; raw throttle so a rev-matched downshift's RPM blip (already off the power,\n"
"; about to brake) can't light it or latch it on through the braking zone.\n"
"ShiftCueMinThrottle=0.5\n"
"; A shift this far (fraction of max RPM) from the target it was aiming at is\n"
"; excluded from learning -- a short-shift or missed gear should not drag the\n"
"; cue. It still shows in the on-screen shift stats.\n"
"ShiftLearnOutlierBand=0.10\n"
"; Only a shift taken at genuine full throttle (raw pedal, checked just\n"
"; before the gear change) counts toward learning or the shift stats -- a\n"
"; deliberate short-shift into a corner is off-throttle by definition and\n"
"; says nothing about the optimal upshift point.\n"
"GearLearnMinThrottle=0.95\n"
"\n"
"; One full on/off flash cycle, in seconds.\n"
"BlinkPeriodSeconds=0.15\n"
"\n"
"; Gear and speed each sit in their OWN dark rounded-corner panel (like the\n"
"; reference in-game HUD's separate segment displays) rather than one shared\n"
"; box. GapPixels is the empty space between the two panels.\n"
"BackgroundEnabled=1\n"
"BackgroundPaddingPixels=26.0\n"
"BackgroundCornerRadiusPixels=14.0\n"
"GapPixels=20.0\n"
"BackgroundR=0.02\n"
"BackgroundG=0.02\n"
"BackgroundB=0.02\n"
"BackgroundA=0.75\n"
"\n"
"; Thin bezel outline around each panel, matching the pedal trace's.\n"
"BorderEnabled=0\n"
"BorderThicknessPixels=2.0\n"
"BorderColorR=0.5\n"
"BorderColorG=0.5\n"
"BorderColorB=0.5\n"
"\n"
"; Speed, drawn in its own panel to the right of the gear panel.\n"
"SpeedEnabled=1\n"
"SpeedUnitsKmh=1\n"
"SpeedTextHeightPixels=96.0\n"
"SpeedColorR=1.0\n"
"SpeedColorG=1.0\n"
"SpeedColorB=1.0\n"
"DividerColorR=0.4\n"
"DividerColorG=0.4\n"
"DividerColorB=0.4\n"
"\n"
"; Whole dashboard (gear + speed + pit box) fades out under the same\n"
"; condition as the pedal trace -- stationary AND off both pedals -- and\n"
"; back in as soon as either is no longer true.\n"
"IdleFadeSeconds=2.0\n"
"\n"
"; Rolling \"how am I actually shifting\" readout, below the dashboard --\n"
"; average and 1st-percentile shift point, both as a percentage of maxRpm.\n"
"; Needs at least one observed upshift to show anything.\n"
"ShowShiftStats=1\n"
"ShiftStatsTextHeightPixels=20.0\n"
"ShiftStatsGapPixels=8.0\n"
"ShiftStatsColorR=0.8\n"
"ShiftStatsColorG=0.8\n"
"ShiftStatsColorB=0.8\n"
"; -1,-1 = auto-anchor just below the gear panel until first dragged.\n"
"ShiftStatsPosXFraction=-1.0\n"
"ShiftStatsPosYFraction=-1.0\n"
"ShiftStatsScale=1.0\n"
"ShiftStatsBgEnabled=1\n"
"ShiftStatsBgPaddingPixels=8.0\n"
"ShiftStatsBgCornerRadiusPixels=8.0\n"
"ShiftStatsBgColorR=0.08\n"
"ShiftStatsBgColorG=0.08\n"
"ShiftStatsBgColorB=0.08\n"
"ShiftStatsBgColorA=0.6\n"
"\n"
"[PitLimiter]\n"
"; Only drawn while the limiter is actually engaged.\n"
"Enabled=0\n"
"\n"
"; Horizontal position of the flashing ENGAGE PIT LIMITER reminder. Its\n"
"; vertical position is EngageWarningYFraction, further down this section;\n"
"; it had no X at all before, so it was welded to screen centre.\n"
"EngageWarningXFraction=0.5\n"
"\n"
"; Horizontal position is automatic -- flush against whichever of the\n"
"; gear/speed dashboard's panels ends up rightmost. This is a small\n"
"; vertical-only nudge, in fractions of screen height.\n"
"OffsetYFraction=0.0\n"
"\n"
"TextHeightPixels=56.0\n"
"PaddingPixels=18.0\n"
"\n"
"TextColorR=1.0\n"
"TextColorG=1.0\n"
"TextColorB=1.0\n"
"\n"
"; Fallback only -- normally learned automatically from the limiter's own\n"
"; cap and persisted per car+track; this is what's used before a value has\n"
"; been learned or loaded for the current combo.\n"
"SpeedLimitKmh=80.0\n"
"\n"
"; Green at/under the limit, fading through orange, to red as speed goes\n"
"; further over it -- blinks once over by OverLimitFarKmh or more rather\n"
"; than at the first km/h over.\n"
"FillColorR=0.15\n"
"FillColorG=1.0\n"
"FillColorB=0.15\n"
"MidColorR=1.0\n"
"MidColorG=0.6\n"
"MidColorB=0.1\n"
"OverLimitColorR=1.0\n"
"OverLimitColorG=0.15\n"
"OverLimitColorB=0.15\n"
"OverLimitFarKmh=15.0\n"
"OverLimitFlashPeriodSeconds=0.2\n"
"\n"
"; Two extra lines beneath \"PIT\": the limit itself, and current speed minus\n"
"; that limit (positive = still over, negative = under) -- same units as the\n"
"; main speed readout (GearDisplay's SpeedUnitsKmh).\n"
"ShowLimitAndDelta=1\n"
"InfoTextHeightPixels=24.0\n"
"\n"
"; The SDK's pit location does not land exactly on the pit-lane entry line,\n"
"; which the engage reminder above uses. This many metres is added to it\n"
"; (positive = later) until a track has its own offset, learned automatically\n"
"; the first time you enter that pit lane (PitEntryOffsets folder).\n"
"EntryOffsetMeters=0.0\n"
"\n"
"[LapDelta]\n"
"; Live delta-to-best-lap, compared at the SAME TRACK DISTANCE (not the same\n"
"; elapsed time). Two references are shown stacked, each labeled:\n"
"; \"S\" = fastest lap so far THIS SESSION (forgotten when the session ends).\n"
"; \"B\" = the all-time best for this car+track, saved to disk (survives a\n"
"; restart, unlike the game's own session-only best time) and only ever\n"
"; replaced by a genuinely faster complete lap. Saved under a \"BestLaps\"\n"
"; folder next to this INI.\n"
"Enabled=1\n"
"\n"
"CenterXFraction=0.5\n"
"CenterYFraction=0.225\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"Scale=1.0\n"
"; A third of the screen. The [MiniSector] strips above this bar take their\n"
"; width from here by default, so changing this moves all three together.\n"
"BarWidthFraction=0.3333\n"
"BarHeightPixels=54.0\n"
"NumberGapPixels=6.0\n"
"TextHeightPixels=41.6\n"
"; Gap between the stacked Session and Best rows.\n"
"RowGapPixels=10.0\n"
"; The PB row and the DRS indicator each have their own position, so they\n"
"; can sit apart from the session-best bar. -1 means stacked where it\n"
"; always was; the first drag resolves that into a real value.\n"
"; Independent size multipliers for the PB row and the DRS box, which are\n"
"; separately positionable and so get their own corner handles in layout\n"
"; mode. Scale above covers the delta bar itself only.\n"
"BestScale=1.0\n"
"DrsScale=1.0\n"
"; The bar is RED while it is growing and GREEN while it is shrinking --\n"
"; judged on the drawn value against where it was ColorWindowSeconds ago,\n"
"; so the colour tracks what you can actually see the bar doing. Changes\n"
"; smaller than ColorDeadbandSeconds across that window count as static\n"
"; and hold the previous colour, so a flat delta does not strobe.\n"
"ColorWindowSeconds=0.4\n"
"ColorDeadbandSeconds=0.004\n"
"BestCenterXFraction=-1\n"
"BestCenterYFraction=-1\n"
"DrsCenterXFraction=-1\n"
"DrsCenterYFraction=-1\n"
"; Delta beyond this many seconds fills the bar completely -- also the\n"
"; window's FLOOR for the auto-sizing below.\n"
"MaxBarSeconds=1.0\n"
"; Auto-sizing: the window resets to the floor (MaxBarSeconds above) at the\n"
"; start of every lap, then grows live in whole-second steps only if that\n"
"; lap's own delta actually needs it. Never grows past\n"
"; BarWindowMaxSeconds below. 0 = fixed MaxBarSeconds-wide window, like\n"
"; before.\n"
"AutoBarWindowEnabled=1\n"
"BarWindowMaxSeconds=10.0\n"
"\n"
"; The Best row has no bar at all -- just a plain number, coloured green\n"
"; when ahead of the reference, red when behind.\n"
"NumberAheadColorR=0.15\n"
"NumberAheadColorG=1.0\n"
"NumberAheadColorB=0.15\n"
"NumberBehindColorR=1.0\n"
"NumberBehindColorG=0.15\n"
"NumberBehindColorB=0.15\n"
"\n"
"; The Session row's number is drawn centred ON its bar instead, always\n"
"; plain white -- the bar's own fill colour (trend-based) already carries\n"
"; the gaining/losing signal.\n"
"BarNumberColorR=1.0\n"
"BarNumberColorG=1.0\n"
"BarNumberColorB=1.0\n"
"\n"
"; A thin dark outline around that number -- without it, half the text sits\n"
"; over the bar's plain grey background and half over its coloured fill,\n"
"; giving it two different, inconsistent backgrounds at once.\n"
"BarNumberOutlineEnabled=1\n"
"BarNumberOutlineColorR=0.0\n"
"BarNumberOutlineColorG=0.0\n"
"BarNumberOutlineColorB=0.0\n"
"BarNumberOutlineThicknessPixels=2.0\n"
"\n"
"; The BAR's colour follows the TREND instead: flat green while the delta\n"
"; is moving more negative (gaining time), flat red while moving more\n"
"; positive (losing time) -- independent of whether the delta itself is\n"
"; still ahead or behind, so bleeding back an advantage shows red on the\n"
"; bar immediately. Single flat colour each way, no brightness scale.\n"
"GainingColorR=0.15\n"
"GainingColorG=1.0\n"
"GainingColorB=0.15\n"
"LosingColorR=1.0\n"
"LosingColorG=0.15\n"
"LosingColorB=0.15\n"
"; How long a gaining<->losing colour change takes to reach its target,\n"
"; instead of snapping instantly -- avoids a strobe when the trend flickers\n"
"; right at the boundary.\n"
"BarColorFadeSeconds=1.0\n"
"\n"
"; The trend is measured as (delta now - delta this many seconds ago) / that\n"
"; same span -- a longer window reads as a real gain/loss trend, a shorter\n"
"; one is noisier but reacts faster.\n"
"TrendWindowSeconds=2.0\n"
"; Hold the delta shown AT the line for this many seconds after crossing,\n"
"; so the completed lap can be read on the home straight instead of the\n"
"; number resetting the instant it becomes meaningful. 0 disables.\n"
"FreezeAfterLineSeconds=3.0\n"
"\n"
"; Dark grey panel behind the bar+number, same idea as the FPS/perf stats\n"
"; box -- keeps it readable over any track scenery.\n"
"BackgroundEnabled=1\n"
"BackgroundPaddingPixels=4.0\n"
"BackgroundCornerRadiusPixels=8.0\n"
"BackgroundR=0.08\n"
"BackgroundG=0.08\n"
"BackgroundB=0.08\n"
"BackgroundA=0.6\n"
"\n"
"[DrsCue]\n"
"; Double-beep the moment DRS/the rear flap becomes legal to activate --\n"
"; once, right when it becomes allowed, not a repeated nag while it stays\n"
"; allowed.\n"
"Enabled=1\n"
"BeepFrequencyHz=1200\n"
"BeepDurationMs=90\n"
"GapMs=80\n"
"\n"
"[Text]\n"
"; Translucent plate drawn behind text so it stays readable over bright\n"
"; track, sky or a white car. Applied by the renderer to ALL text, so\n"
"; anything added later gets one automatically; widgets that already draw\n"
"; their own background panel suppress it rather than double-darkening.\n"
"BackgroundEnabled=1\n"
"BackgroundColorR=0.04\n"
"BackgroundColorG=0.04\n"
"BackgroundColorB=0.04\n"
"BackgroundAlpha=0.55\n"
"BackgroundPaddingPixels=6.0\n"
"BackgroundCornerRadiusPixels=5.0\n"
"; Font for all text: Auto (Consolas, or the built-in DejaVu Sans Mono\n"
"; where Consolas isn't installed, e.g. Linux/Proton), Consolas,\n"
"; DejaVu Sans Mono, Lucida Console or Courier New. A font that isn't\n"
"; installed falls back to the built-in one. FontWeight: Light, Normal or\n"
"; Bold. Both can also be changed by clicking them in the shortcut list.\n"
"Font=Auto\n"
"FontWeight=Light\n"
"\n"
"[Clock]\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"Scale=1.0\n"
"; Wall-clock time of day, top right, styled to match the frametime stats.\n"
"Enabled=1\n"
"ShowSeconds=0\n"
"PosXFraction=0.0\n"
"PosYFraction=1.0\n"
"TextHeightPixels=34.0\n"
"ColorR=0.85\n"
"ColorG=0.85\n"
"ColorB=0.85\n"
"\n"
"[Shortcuts]\n"
"; Per-element visibility toggles, each Ctrl+Shift+<key>. A key is either\n"
"; a single character (letter or digit, case does not matter) or F1..F12.\n"
"; Ctrl+Shift+<List> shows the whole list on screen, and the same hint is\n"
"; advertised briefly whenever you start driving. With the list open and\n"
"; the mouse cursor visible you can also just CLICK a row to toggle it.\n"
";\n"
"; F9 is deliberately unused: it is bound to rFactor 2 own Toggle Cursor\n"
"; control, which fires on the bare key regardless of modifiers, so\n"
"; Ctrl+Shift+F9 would do two things at once. 7, 8, 9 and 0 are free.\n"
"Enabled=1\n"
"Radar=F1\n"
"PedalTrace=F2\n"
"Perf=F3\n"
"Gear=F4\n"
"LapDelta=F5\n"
"BestDelta=F6\n"
"MiniSector=F7\n"
"RelativeTiming=F8\n"
"FuelTyre=F10\n"
"BrakeCue=F11\n"
"; DRS beep AND the on-screen \"OPEN DRS\"/\"DRS ENGAGED\" sign, together --\n"
"; one switch for the whole feature.\n"
"Drs=D\n"
"; List and LayoutMode are the two most important bindings -- everything\n"
"; else is reachable from the on-screen list, but these two are how you\n"
"; get to it and how you rearrange the HUD, so they sit first on 1 and 2.\n"
"List=1\n"
"LayoutMode=2\n"
"; Master on/off for the whole overlay -- everything this plugin draws.\n"
"MasterToggle=H\n"
"ShowOverlay=1\n"
"Clock=3\n"
"SideBar=S\n"
"; Independent toggles for the three FuelTyre boxes -- FuelTyre above\n"
"; remains a master switch for all three at once.\n"
"FuelPanel=N\n"
"TyresPanel=Y\n"
"BrakeBiasPanel=K\n"
"TyreLockup=L\n"
"ShiftStats=V\n"
"Leaderboard=Unassigned\n"
"ReplayHeader=Unassigned\n"
"FocusDelta=Unassigned\n"
"\n"
"; Actions rather than toggles. ResetPersonalBest is destructive (it\n"
"; deletes the saved best-lap file) so it must be pressed TWICE within\n"
"; ResetConfirmSeconds.\n"
"ResetSessionBest=5\n"
"ResetPersonalBest=6\n"
"ResetConfirmSeconds=4.0\n"
"; The transient message box (toggle confirmations, startup hint, the\n"
"; INI-write warning).\n"
"MessageCenterXFraction=0.5\n"
"MessageCenterYFraction=0.62\n"
"; The shortcut list. -1 auto-anchors it below the fuel/tyre panel, which\n"
"; is what it did before it was draggable. Dragging it fixes it in place\n"
"; and gives up that auto-anchoring.\n"
"ListPosXFraction=-1\n"
"ListPosYFraction=-1\n"
"; Panel size multiplier, set by dragging the corner handle in layout\n"
"; mode and clamped there to 0.5-2.5. Scales text, padding\n"
"; and gaps together, in proportion.\n"
"ListScale=1.0\n"
"MessageScale=1.0\n"
"\n"
"; Which elements are shown. These are REWRITTEN by the plugin whenever\n"
"; you toggle something with a hotkey, so the last state is restored the\n"
"; next time you start the game.\n"
"ShowRadar=1\n"
"ShowPedalTrace=1\n"
"ShowPerf=1\n"
"ShowGear=1\n"
"ShowLapDelta=1\n"
"ShowBestDelta=1\n"
"ShowMiniSector=0\n"
"ShowRelativeTiming=1\n"
"ShowFuelTyre=1\n"
"ShowBrakeCue=0\n"
"ShowDrs=1\n"
"ShowClock=1\n"
"ShowSideBar=1\n"
"ShowLeaderboard=1\n"
"ShowReplayHeader=1\n"
"ShowFocusDelta=1\n"
"StartupHintSeconds=6.0\n"
"\n"
"[Layout]\n"
"; The alignment grid drawn ONLY while layout mode is on.\n"
"; Minor lines are faint on purpose -- at 4K a 100px spacing is already\n"
"; ~38 columns, and anything stronger reads as a grey wash. The major\n"
"; line is what the eye counts by; the screen-centre pair is brightest.\n"
"GridEnabled=1\n"
"GridMinorPixels=100\n"
"GridMajorPixels=500\n"
"GridColorR=1.0\n"
"GridColorG=1.0\n"
"GridColorB=1.0\n"
"GridMinorAlpha=0.10\n"
"GridMajorAlpha=0.28\n"
"GridCenterAlpha=0.55\n"
"; The mode banner is draggable too -- it sits across the top by default,\n"
"; which is where some panels want to be.\n"
"BannerPosXFraction=0.5\n"
"BannerPosYFraction=0.12\n"
"\n"
"[Performance]\n"
"; One shared cadence for the perf overlay's own text refresh, the GPU\n"
"; busy-time PDH poll, and the CPU-load CSV recorder's sampling -- lower it\n"
"; to cut background measurement overhead, raise it for fresher readouts.\n"
"; Clamped to 0.2-30.0. Does not affect on-screen smoothing/fades, which\n"
"; are about how things look, not how much work gets done.\n"
"UpdateHz=2.0\n"
"\n"
"[RelativeTiming]\n"
"; Tyre compound column for the driving timing table, using the F1 colour\n"
"; code: S red, M yellow, H white, I green, W blue. An unrecognised mod\n"
"; compound shows its first letter in grey.\n"
";\n"
"; Compound lives in TELEMETRY, and the game normally sends this plugin\n"
"; only your own car -- so turning this on also widens telemetry to the\n"
"; whole field, costing roughly 3000 extra callbacks a second while\n"
"; driving. The work per callback is deliberately tiny: an ID compare,\n"
"; with the compound itself read only once a lap.\n"
"ShowTyre=0\n"
"; How many cars the table shows at once -- also adjustable live from a\n"
"; slider in the shortcuts list (Ctrl+Shift+<List>). 3-12.\n"
"VisibleCars=6\n"
"\n"
;

    float ReadFloat(const char* section, const char* key, float def)
    {
        char buf[64];
        char defStr[64];
        sprintf_s(defStr, "%g", def);
        GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), s_iniPath);
        return static_cast<float>(atof(buf));
    }

    bool ReadBool(const char* section, const char* key, bool def)
    {
        return GetPrivateProfileIntA(section, key, def ? 1 : 0, s_iniPath) != 0;
    }

    int ReadInt(const char* section, const char* key, int def)
    {
        return GetPrivateProfileIntA(section, key, def, s_iniPath);
    }

    // Free text, for the one setting that is a path rather than a number.
    // Writes back into the caller's own buffer, leaving it untouched (i.e.
    // still holding the compiled-in default) if the key is absent or blank.
    void ReadString(const char* section, const char* key, char* buf, size_t bufSize)
    {
        char tmp[512];
        GetPrivateProfileStringA(section, key, "", tmp, sizeof(tmp), s_iniPath);
        if (tmp[0])
            strncpy_s(buf, bufSize, tmp, _TRUNCATE);
    }

    // A single letter, for the keyboard shortcuts. Takes the first character
    // of whatever is there and upper-cases it, so "r", "R" and even "Radar"
    // all resolve to 'R' -- a virtual-key code is what GetAsyncKeyState
    // wants, and for A-Z that is just the uppercase ASCII value. Anything
    // that isn't a letter falls back to the default rather than producing a
    // shortcut that can never be pressed.
    // Shortcut keys are stored as VIRTUAL-KEY CODES, which is what
    // GetAsyncKeyState wants. For A-Z and 0-9 the code IS the uppercase
    // ASCII value, so those round-trip as a single character. Function keys
    // have no printable character at all, so they are written in the INI as
    // "F1".."F12" and translated here.
    int ReadKey(const char* section, const char* key, int def)
    {
        char defStr[16];
        Config::KeyName(def, defStr, sizeof(defStr));

        char buf[32];
        GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), s_iniPath);

        // Trim whitespace, which is easy to leave behind when hand-editing.
        char* start = buf;
        while (*start == ' ' || *start == '\t') ++start;
        char* end = start;
        while (*end) ++end;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;

        // Explicit "unassigned" markers, checked as a FULL match rather than
        // by first character: the fallback below takes just start[0] for
        // anything else (so "r", "R" and even "Radar" all resolve to 'R'),
        // which means "Unassigned" -- the exact string Config::KeyName now
        // writes for an unassigned key -- would otherwise silently parse as
        // the letter 'U'. An empty value counts too, since that's what a
        // hand-edited "Key=" with nothing after it leaves.
        if (start[0] == 0 || _stricmp(start, "Unassigned") == 0 || _stricmp(start, "None") == 0)
            return 0;

        if ((start[0] == 'F' || start[0] == 'f') && start[1] >= '0' && start[1] <= '9')
        {
            const int n = atoi(start + 1);
            if (n >= 1 && n <= 12)
                return VK_F1 + (n - 1);
            return def;
        }

        const char c = start[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;   // VK codes for digits are their ASCII values
        return def;
    }
}

void Config::Load()
{
    // Resolve our own DLL's path (no DllMain needed -- this finds the module
    // containing this very function's code) so the INI sits next to the DLL
    // regardless of where rFactor 2 is installed.
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&Config::Load), &hSelf);

    char dllPath[MAX_PATH];
    GetModuleFileNameA(hSelf, dllPath, MAX_PATH);

    strcpy_s(s_iniPath, dllPath);
    char* lastSlash = strrchr(s_iniPath, '\\');
    if (lastSlash)
        *(lastSlash + 1) = '\0';
    strcat_s(s_iniPath, "rF2SkrOverlayPublic.ini");

    if (GetFileAttributesA(s_iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, s_iniPath, "w") == 0 && f)
        {
            fputs(kDefaultIni, f);
            fclose(f);
            Log::Write("Config: wrote default INI to %s", s_iniPath);
        }
        else
        {
            Log::Write("Config: could not create %s, using built-in defaults", s_iniPath);
        }
    }

    RadarConfig& r = g_radarConfig;
    TextBackgroundConfig& tb = g_textBackgroundConfig;
    tb.enabled = ReadBool("Text", "BackgroundEnabled", tb.enabled);
    tb.r = ReadFloat("Text", "BackgroundColorR", tb.r);
    tb.g = ReadFloat("Text", "BackgroundColorG", tb.g);
    tb.b = ReadFloat("Text", "BackgroundColorB", tb.b);
    tb.alpha = ReadFloat("Text", "BackgroundAlpha", tb.alpha);
    tb.paddingPx = ReadFloat("Text", "BackgroundPaddingPixels", tb.paddingPx);
    tb.cornerRadiusPx = ReadFloat("Text", "BackgroundCornerRadiusPixels", tb.cornerRadiusPx);
    {
        // By name; anything unrecognised keeps the default.
        char name[64] = "";
        ReadString("Text", "Font", name, sizeof(name));
        for (int i = 0; i < Renderer::kFontCount; ++i)
            if (_stricmp(name, Renderer::kFontNames[i]) == 0) g_textFontConfig.font = i;
        name[0] = 0;
        ReadString("Text", "FontWeight", name, sizeof(name));
        for (int i = 0; i < Renderer::kWeightCount; ++i)
            if (_stricmp(name, Renderer::kWeightNames[i]) == 0) g_textFontConfig.weight = i;
    }

    ClockConfig& clk = g_clockConfig;
    clk.enabled = ReadBool("Clock", "Enabled", clk.enabled);
    clk.showSeconds = ReadBool("Clock", "ShowSeconds", clk.showSeconds);
    clk.posXFraction = ReadFloat("Clock", "PosXFraction", clk.posXFraction);
    clk.posYFraction = ReadFloat("Clock", "PosYFraction", clk.posYFraction);
    clk.scale = ReadFloat("Clock", "Scale", clk.scale);

    clk.textHeightPx = ReadFloat("Clock", "TextHeightPixels", clk.textHeightPx);
    clk.r = ReadFloat("Clock", "ColorR", clk.r);
    clk.g = ReadFloat("Clock", "ColorG", clk.g);
    clk.b = ReadFloat("Clock", "ColorB", clk.b);

    DllCostConfig& cost = g_dllCostConfig;
    cost.posXFraction = ReadFloat("DllCost", "PosXFraction", cost.posXFraction);
    cost.posYFraction = ReadFloat("DllCost", "PosYFraction", cost.posYFraction);
    cost.scale = ReadFloat("DllCost", "Scale", cost.scale);
    cost.textHeightPx = ReadFloat("DllCost", "TextHeightPixels", cost.textHeightPx);
    cost.barWidthPx = ReadFloat("DllCost", "BarWidthPixels", cost.barWidthPx);
    cost.barHeightPx = ReadFloat("DllCost", "BarHeightPixels", cost.barHeightPx);
    cost.labelMinShare = ReadFloat("DllCost", "LabelMinShare", cost.labelMinShare);


    ShortcutConfig& sc = g_shortcutConfig;
    sc.enabled = ReadBool("Shortcuts", "Enabled", sc.enabled);
    sc.radar = ReadKey("Shortcuts", "Radar", sc.radar);
    sc.pedalTrace = ReadKey("Shortcuts", "PedalTrace", sc.pedalTrace);
    sc.perf = ReadKey("Shortcuts", "Perf", sc.perf);
    sc.gear = ReadKey("Shortcuts", "Gear", sc.gear);
    sc.lapDelta = ReadKey("Shortcuts", "LapDelta", sc.lapDelta);
    sc.miniSector = ReadKey("Shortcuts", "MiniSector", sc.miniSector);
    sc.relativeTiming = ReadKey("Shortcuts", "RelativeTiming", sc.relativeTiming);
    sc.fuelTyre = ReadKey("Shortcuts", "FuelTyre", sc.fuelTyre);
    sc.brakeCue = ReadKey("Shortcuts", "BrakeCue", sc.brakeCue);
    sc.drs = ReadKey("Shortcuts", "Drs", sc.drs);
    sc.clock = ReadKey("Shortcuts", "Clock", sc.clock);
    sc.sideBar = ReadKey("Shortcuts", "SideBar", sc.sideBar);
    sc.fuelPanel = ReadKey("Shortcuts", "FuelPanel", sc.fuelPanel);
    sc.tyresPanel = ReadKey("Shortcuts", "TyresPanel", sc.tyresPanel);
    sc.brakeBiasPanel = ReadKey("Shortcuts", "BrakeBiasPanel", sc.brakeBiasPanel);
    sc.tyreLockup = ReadKey("Shortcuts", "TyreLockup", sc.tyreLockup);
    sc.shiftStats = ReadKey("Shortcuts", "ShiftStats", sc.shiftStats);
    sc.tyreTemps = ReadKey("Shortcuts", "TyreTemps", sc.tyreTemps);
    sc.ffbGraph = ReadKey("Shortcuts", "FfbGraph", sc.ffbGraph);
    sc.trackMap = ReadKey("Shortcuts", "TrackMap", sc.trackMap);
    sc.tyreWearPlots = ReadKey("Shortcuts", "TyreWearPlots", sc.tyreWearPlots);
    sc.dllCost = ReadKey("Shortcuts", "DllCost", sc.dllCost);
    sc.coverBoxes = ReadKey("Shortcuts", "CoverBoxes", sc.coverBoxes);
    sc.list = ReadKey("Shortcuts", "List", sc.list);
    sc.bestDelta = ReadKey("Shortcuts", "BestDelta", sc.bestDelta);
    sc.resetSessionBest = ReadKey("Shortcuts", "ResetSessionBest", sc.resetSessionBest);
    sc.resetPersonalBest = ReadKey("Shortcuts", "ResetPersonalBest", sc.resetPersonalBest);
    sc.layoutMode = ReadKey("Shortcuts", "LayoutMode", sc.layoutMode);
    sc.masterToggle = ReadKey("Shortcuts", "MasterToggle", sc.masterToggle);

    LayoutConfig& lay = g_layoutConfig;
    lay.gridEnabled = ReadBool("Layout", "GridEnabled", lay.gridEnabled);
    lay.gridMinorPx = ReadFloat("Layout", "GridMinorPixels", lay.gridMinorPx);
    lay.gridMajorPx = ReadFloat("Layout", "GridMajorPixels", lay.gridMajorPx);
    lay.gridR = ReadFloat("Layout", "GridColorR", lay.gridR);
    lay.gridG = ReadFloat("Layout", "GridColorG", lay.gridG);
    lay.gridB = ReadFloat("Layout", "GridColorB", lay.gridB);
    lay.gridMinorAlpha = ReadFloat("Layout", "GridMinorAlpha", lay.gridMinorAlpha);
    lay.gridMajorAlpha = ReadFloat("Layout", "GridMajorAlpha", lay.gridMajorAlpha);
    lay.gridCenterAlpha = ReadFloat("Layout", "GridCenterAlpha", lay.gridCenterAlpha);
    lay.bannerPosXFraction = ReadFloat("Layout", "BannerPosXFraction", lay.bannerPosXFraction);
    lay.bannerPosYFraction = ReadFloat("Layout", "BannerPosYFraction", lay.bannerPosYFraction);
    sc.showRadar = ReadBool("Shortcuts", "ShowRadar", sc.showRadar);
    sc.showPedalTrace = ReadBool("Shortcuts", "ShowPedalTrace", sc.showPedalTrace);
    sc.showPerf = ReadBool("Shortcuts", "ShowPerf", sc.showPerf);
    sc.showGear = ReadBool("Shortcuts", "ShowGear", sc.showGear);
    sc.showLapDelta = ReadBool("Shortcuts", "ShowLapDelta", sc.showLapDelta);
    sc.showBestDelta = ReadBool("Shortcuts", "ShowBestDelta", sc.showBestDelta);
    sc.showMiniSector = ReadBool("Shortcuts", "ShowMiniSector", sc.showMiniSector);
    sc.showRelativeTiming = ReadBool("Shortcuts", "ShowRelativeTiming", sc.showRelativeTiming);
    sc.showFuelTyre = ReadBool("Shortcuts", "ShowFuelTyre", sc.showFuelTyre);
    sc.showBrakeCue = ReadBool("Shortcuts", "ShowBrakeCue", sc.showBrakeCue);
    sc.showDrs = ReadBool("Shortcuts", "ShowDrs", sc.showDrs);
    sc.showClock = ReadBool("Shortcuts", "ShowClock", sc.showClock);
    sc.showSideBar = ReadBool("Shortcuts", "ShowSideBar", sc.showSideBar);
    sc.showFuelPanel = ReadBool("Shortcuts", "ShowFuelPanel", sc.showFuelPanel);
    sc.showTyresPanel = ReadBool("Shortcuts", "ShowTyresPanel", sc.showTyresPanel);
    sc.showBrakeBiasPanel = ReadBool("Shortcuts", "ShowBrakeBiasPanel", sc.showBrakeBiasPanel);
    sc.showTyreLockup = ReadBool("Shortcuts", "ShowTyreLockup", sc.showTyreLockup);
    sc.showShiftStats = ReadBool("Shortcuts", "ShowShiftStats", sc.showShiftStats);
    sc.showTyreTemps = ReadBool("Shortcuts", "ShowTyreTemps", sc.showTyreTemps);
    sc.showFfbGraph = ReadBool("Shortcuts", "ShowFfbGraph", sc.showFfbGraph);
    sc.showTrackMap = ReadBool("Shortcuts", "ShowTrackMap", sc.showTrackMap);
    sc.showTyreWearPlots = ReadBool("Shortcuts", "ShowTyreWearPlots", sc.showTyreWearPlots);
    sc.showDllCost = ReadBool("Shortcuts", "ShowDllCost", sc.showDllCost);
    sc.showCoverBoxes = ReadBool("Shortcuts", "ShowCoverBoxes", sc.showCoverBoxes);
    sc.showOverlay = ReadBool("Shortcuts", "ShowOverlay", sc.showOverlay);
    sc.resetConfirmSeconds = ReadFloat("Shortcuts", "ResetConfirmSeconds", sc.resetConfirmSeconds);
    sc.messageCenterXFraction = ReadFloat("Shortcuts", "MessageCenterXFraction", sc.messageCenterXFraction);
    sc.messageCenterYFraction = ReadFloat("Shortcuts", "MessageCenterYFraction", sc.messageCenterYFraction);
    sc.listPosXFraction = ReadFloat("Shortcuts", "ListPosXFraction", sc.listPosXFraction);
    sc.listPosYFraction = ReadFloat("Shortcuts", "ListPosYFraction", sc.listPosYFraction);
    sc.listScale = ReadFloat("Shortcuts", "ListScale", sc.listScale);
    sc.messageScale = ReadFloat("Shortcuts", "MessageScale", sc.messageScale);
    sc.startupHintSeconds = ReadFloat("Shortcuts", "StartupHintSeconds", sc.startupHintSeconds);

    r.enabled = ReadBool("Radar", "Enabled", r.enabled);
    r.widthFraction = ReadFloat("Radar", "WidthFraction", r.widthFraction);
    r.centerXFraction = ReadFloat("Radar", "CenterXFraction", r.centerXFraction);
    r.centerYFraction = ReadFloat("Radar", "CenterYFraction", r.centerYFraction);
    r.scale = ReadFloat("Radar", "Scale", r.scale);
    r.rangeForwardM = ReadFloat("Radar", "RangeForwardMeters", r.rangeForwardM);
    r.rangeRearM = ReadFloat("Radar", "RangeRearMeters", r.rangeRearM);
    r.rangeSideM = ReadFloat("Radar", "RangeSideMeters", r.rangeSideM);
    r.fadeStartFraction = ReadFloat("Radar", "FadeStartFraction", r.fadeStartFraction);
    r.carHalfWidthM = ReadFloat("Radar", "CarHalfWidthMeters", r.carHalfWidthM);
    r.carHalfLengthM = ReadFloat("Radar", "CarHalfLengthMeters", r.carHalfLengthM);
    r.carCornerRadiusM = ReadFloat("Radar", "CarCornerRadiusMeters", r.carCornerRadiusM);
    r.useRealCarSize = ReadBool("Radar", "UseRealCarSize", r.useRealCarSize);
    r.footprintBufferMeters = ReadFloat("Radar", "FootprintBufferMeters", r.footprintBufferMeters);
    r.playerR = ReadFloat("Radar", "PlayerColorR", r.playerR);
    r.playerG = ReadFloat("Radar", "PlayerColorG", r.playerG);
    r.playerB = ReadFloat("Radar", "PlayerColorB", r.playerB);
    r.otherR = ReadFloat("Radar", "OtherColorR", r.otherR);
    r.otherG = ReadFloat("Radar", "OtherColorG", r.otherG);
    r.otherB = ReadFloat("Radar", "OtherColorB", r.otherB);
    r.dangerR = ReadFloat("Radar", "DangerColorR", r.dangerR);
    r.dangerG = ReadFloat("Radar", "DangerColorG", r.dangerG);
    r.dangerB = ReadFloat("Radar", "DangerColorB", r.dangerB);
    r.dangerAheadM = ReadFloat("Radar", "DangerAheadMeters", r.dangerAheadM);
    r.dangerBehindM = ReadFloat("Radar", "DangerBehindMeters", r.dangerBehindM);
    r.dangerLateralM = ReadFloat("Radar", "DangerLateralMeters", r.dangerLateralM);
    r.dangerFlashEnabled = ReadBool("Radar", "DangerFlashEnabled", r.dangerFlashEnabled);
    r.dangerFlashR = ReadFloat("Radar", "DangerFlashColorR", r.dangerFlashR);
    r.dangerFlashG = ReadFloat("Radar", "DangerFlashColorG", r.dangerFlashG);
    r.dangerFlashB = ReadFloat("Radar", "DangerFlashColorB", r.dangerFlashB);
    r.dangerFlashMaxAlpha = ReadFloat("Radar", "DangerFlashMaxAlpha", r.dangerFlashMaxAlpha);
    r.dangerFlashPeriodSeconds = ReadFloat("Radar", "DangerFlashPeriodSeconds", r.dangerFlashPeriodSeconds);
    r.logOrientationDebug = ReadBool("Radar", "LogOrientationDebug", r.logOrientationDebug);
    r.sideBarWidthFraction = ReadFloat("Radar", "SideBarWidthFraction", r.sideBarWidthFraction);
    r.sideBarScale = ReadFloat("Radar", "SideBarScale", r.sideBarScale);
    r.sideBarOpacityMultiplier = ReadFloat("Radar", "SideBarOpacityMultiplier", r.sideBarOpacityMultiplier);
    r.sideBarR = ReadFloat("Radar", "SideBarColorR", r.sideBarR);
    r.sideBarG = ReadFloat("Radar", "SideBarColorG", r.sideBarG);
    r.sideBarB = ReadFloat("Radar", "SideBarColorB", r.sideBarB);
    r.sideBarNearM = ReadFloat("Radar", "SideBarNearMeters", r.sideBarNearM);
    r.sideBarAlphaNear = ReadFloat("Radar", "SideBarAlphaNear", r.sideBarAlphaNear);
    r.sideBarAlphaFar = ReadFloat("Radar", "SideBarAlphaFar", r.sideBarAlphaFar);
    r.sideBarInnerEdgeAlpha = ReadFloat("Radar", "SideBarInnerEdgeAlpha", r.sideBarInnerEdgeAlpha);
    r.sideBarVerticalEndFade = ReadFloat("Radar", "SideBarVerticalEndFade", r.sideBarVerticalEndFade);
    r.sideBarVerticalFadeGamma = ReadFloat("Radar", "SideBarVerticalFadeGamma", r.sideBarVerticalFadeGamma);
    r.sideBarWidthEndTaper = ReadFloat("Radar", "SideBarWidthEndTaper", r.sideBarWidthEndTaper);
    r.sideBarFadeInSeconds = ReadFloat("Radar", "SideBarFadeInSeconds", r.sideBarFadeInSeconds);
    r.sideBarFadeOutSeconds = ReadFloat("Radar", "SideBarFadeOutSeconds", r.sideBarFadeOutSeconds);
    r.sideBarFadeToGreenEnabled = ReadBool("Radar", "SideBarFadeToGreenEnabled", r.sideBarFadeToGreenEnabled);
    r.sideBarFadeToGreenR = ReadFloat("Radar", "SideBarFadeToGreenColorR", r.sideBarFadeToGreenR);
    r.sideBarFadeToGreenG = ReadFloat("Radar", "SideBarFadeToGreenColorG", r.sideBarFadeToGreenG);
    r.sideBarFadeToGreenB = ReadFloat("Radar", "SideBarFadeToGreenColorB", r.sideBarFadeToGreenB);
    r.sideBarTooCloseFlashEnabled = ReadBool("Radar", "SideBarTooCloseFlashEnabled", r.sideBarTooCloseFlashEnabled);
    r.sideBarTooCloseMeters = ReadFloat("Radar", "SideBarTooCloseMeters", r.sideBarTooCloseMeters);
    r.sideBarTooCloseMetersMax = ReadFloat("Radar", "SideBarTooCloseMetersMax", r.sideBarTooCloseMetersMax);
    r.sideBarTooCloseFlashHz = ReadFloat("Radar", "SideBarTooCloseFlashHz", r.sideBarTooCloseFlashHz);
    r.sideBarTooCloseFlashHzMax = ReadFloat("Radar", "SideBarTooCloseFlashHzMax", r.sideBarTooCloseFlashHzMax);
    r.sideBarTooCloseFlashR = ReadFloat("Radar", "SideBarTooCloseFlashColorR", r.sideBarTooCloseFlashR);
    r.sideBarTooCloseFlashG = ReadFloat("Radar", "SideBarTooCloseFlashColorG", r.sideBarTooCloseFlashG);
    r.sideBarTooCloseFlashB = ReadFloat("Radar", "SideBarTooCloseFlashColorB", r.sideBarTooCloseFlashB);

    PedalConfig& p = g_pedalConfig;
    p.enabled = ReadBool("PedalTrace", "Enabled", p.enabled);
    p.widthFraction = ReadFloat("PedalTrace", "WidthFraction", p.widthFraction);
    p.heightFraction = ReadFloat("PedalTrace", "HeightFraction", p.heightFraction);
    p.centerXFraction = ReadFloat("PedalTrace", "CenterXFraction", p.centerXFraction);
    p.centerYFraction = ReadFloat("PedalTrace", "CenterYFraction", p.centerYFraction);
    p.scale = ReadFloat("PedalTrace", "Scale", p.scale);
    p.bgR = ReadFloat("PedalTrace", "BackgroundR", p.bgR);
    p.bgG = ReadFloat("PedalTrace", "BackgroundG", p.bgG);
    p.bgB = ReadFloat("PedalTrace", "BackgroundB", p.bgB);
    p.bgA = ReadFloat("PedalTrace", "BackgroundA", p.bgA);
    p.bgCornerRadiusPx = ReadFloat("PedalTrace", "BackgroundCornerRadiusPixels", p.bgCornerRadiusPx);
    p.borderEnabled = ReadBool("PedalTrace", "BorderEnabled", p.borderEnabled);
    p.borderThicknessPx = ReadFloat("PedalTrace", "BorderThicknessPixels", p.borderThicknessPx);
    p.borderR = ReadFloat("PedalTrace", "BorderColorR", p.borderR);
    p.borderG = ReadFloat("PedalTrace", "BorderColorG", p.borderG);
    p.borderB = ReadFloat("PedalTrace", "BorderColorB", p.borderB);
    p.throttleRawR = ReadFloat("PedalTrace", "ThrottleRawR", p.throttleRawR);
    p.throttleRawG = ReadFloat("PedalTrace", "ThrottleRawG", p.throttleRawG);
    p.throttleRawB = ReadFloat("PedalTrace", "ThrottleRawB", p.throttleRawB);
    p.throttleActR = ReadFloat("PedalTrace", "ThrottleActualR", p.throttleActR);
    p.throttleActG = ReadFloat("PedalTrace", "ThrottleActualG", p.throttleActG);
    p.throttleActB = ReadFloat("PedalTrace", "ThrottleActualB", p.throttleActB);
    p.brakeRawR = ReadFloat("PedalTrace", "BrakeRawR", p.brakeRawR);
    p.brakeRawG = ReadFloat("PedalTrace", "BrakeRawG", p.brakeRawG);
    p.brakeRawB = ReadFloat("PedalTrace", "BrakeRawB", p.brakeRawB);
    p.brakeActR = ReadFloat("PedalTrace", "BrakeActualR", p.brakeActR);
    p.brakeActG = ReadFloat("PedalTrace", "BrakeActualG", p.brakeActG);
    p.brakeActB = ReadFloat("PedalTrace", "BrakeActualB", p.brakeActB);
    p.lockupR = ReadFloat("PedalTrace", "LockupR", p.lockupR);
    p.lockupG = ReadFloat("PedalTrace", "LockupG", p.lockupG);
    p.lockupB = ReadFloat("PedalTrace", "LockupB", p.lockupB);
    p.spinR = ReadFloat("PedalTrace", "SpinR", p.spinR);
    p.spinG = ReadFloat("PedalTrace", "SpinG", p.spinG);
    p.spinB = ReadFloat("PedalTrace", "SpinB", p.spinB);
    p.slipDeadzone = ReadFloat("PedalTrace", "SlipDeadzone", p.slipDeadzone);
    p.slipFullScale = ReadFloat("PedalTrace", "SlipFullScale", p.slipFullScale);
    p.minSpeedForSlipMs = ReadFloat("PedalTrace", "MinSpeedMetersPerSec", p.minSpeedForSlipMs);
    p.slipIntensityMultiplier = ReadFloat("PedalTrace", "SlipIntensityMultiplier", p.slipIntensityMultiplier);
    p.slipBarHeightScale = ReadFloat("PedalTrace", "SlipBarHeightScale", p.slipBarHeightScale);
    p.steeringR = ReadFloat("PedalTrace", "SteeringR", p.steeringR);
    p.steeringG = ReadFloat("PedalTrace", "SteeringG", p.steeringG);
    p.steeringB = ReadFloat("PedalTrace", "SteeringB", p.steeringB);
    p.idleFadeSeconds = ReadFloat("PedalTrace", "IdleFadeSeconds", p.idleFadeSeconds);
    p.pointSpacingPx = ReadFloat("PedalTrace", "PointSpacingPixels", p.pointSpacingPx);

    BrakeCueConfig& b = g_brakeCueConfig;
    b.enabled = ReadBool("BrakeCue", "Enabled", b.enabled);
    b.brakeThreshold = ReadFloat("BrakeCue", "BrakeThreshold", b.brakeThreshold);
    b.minPointSpacingM = ReadFloat("BrakeCue", "MinPointSpacingMeters", b.minPointSpacingM);
    b.minSpeedDropKmh = ReadFloat("BrakeCue", "MinSpeedDropKmh", b.minSpeedDropKmh);
    if (b.minSpeedDropKmh < BrakeCueConfig::kMinSpeedDropMin) b.minSpeedDropKmh = BrakeCueConfig::kMinSpeedDropMin;
    if (b.minSpeedDropKmh > BrakeCueConfig::kMinSpeedDropMax) b.minSpeedDropKmh = BrakeCueConfig::kMinSpeedDropMax;
    b.leadTimeSec = ReadFloat("BrakeCue", "LeadTimeSeconds", b.leadTimeSec);
    b.beepFrequencyHz = ReadInt("BrakeCue", "BeepFrequencyHz", b.beepFrequencyHz);
    b.beepDurationMs = ReadInt("BrakeCue", "BeepDurationMs", b.beepDurationMs);
    b.reactionThreshold = ReadFloat("BrakeCue", "ReactionThreshold", b.reactionThreshold);
    b.reactionTimeoutSeconds = ReadFloat("BrakeCue", "ReactionTimeoutSeconds", b.reactionTimeoutSeconds);
    b.adaptiveLeadTimeEnabled = ReadBool("BrakeCue", "AdaptiveLeadTimeEnabled", b.adaptiveLeadTimeEnabled);
    b.reactionMinSeconds = ReadFloat("BrakeCue", "ReactionMinSeconds", b.reactionMinSeconds);
    b.reactionMaxSeconds = ReadFloat("BrakeCue", "ReactionMaxSeconds", b.reactionMaxSeconds);
    b.showReactionStats = ReadBool("BrakeCue", "ShowReactionStats", b.showReactionStats);
    b.reactionStatsCenterXFraction = ReadFloat("BrakeCue", "ReactionStatsCenterXFraction", b.reactionStatsCenterXFraction);
    b.reactionStatsCenterYFraction = ReadFloat("BrakeCue", "ReactionStatsCenterYFraction", b.reactionStatsCenterYFraction);
    b.reactionStatsTextHeightPx = ReadFloat("BrakeCue", "ReactionStatsTextHeightPixels", b.reactionStatsTextHeightPx);
    b.reactionStatsScale = ReadFloat("BrakeCue", "ReactionStatsScale", b.reactionStatsScale);
    b.reactionStatsR = ReadFloat("BrakeCue", "ReactionStatsColorR", b.reactionStatsR);
    b.reactionStatsG = ReadFloat("BrakeCue", "ReactionStatsColorG", b.reactionStatsG);
    b.reactionStatsB = ReadFloat("BrakeCue", "ReactionStatsColorB", b.reactionStatsB);
    b.reactionStatsBgEnabled = ReadBool("BrakeCue", "ReactionStatsBackgroundEnabled", b.reactionStatsBgEnabled);
    b.reactionStatsBgPaddingPx = ReadFloat("BrakeCue", "ReactionStatsBackgroundPaddingPixels", b.reactionStatsBgPaddingPx);
    b.reactionStatsBgCornerRadiusPx = ReadFloat("BrakeCue", "ReactionStatsBackgroundCornerRadiusPixels", b.reactionStatsBgCornerRadiusPx);
    b.reactionStatsBgR = ReadFloat("BrakeCue", "ReactionStatsBackgroundR", b.reactionStatsBgR);
    b.reactionStatsBgG = ReadFloat("BrakeCue", "ReactionStatsBackgroundG", b.reactionStatsBgG);
    b.reactionStatsBgB = ReadFloat("BrakeCue", "ReactionStatsBackgroundB", b.reactionStatsBgB);
    b.reactionStatsBgA = ReadFloat("BrakeCue", "ReactionStatsBackgroundA", b.reactionStatsBgA);

    MiniSectorConfig& ms = g_miniSectorConfig;
    ms.enabled = ReadBool("MiniSector", "Enabled", ms.enabled);
    ms.segmentCount = ReadInt("MiniSector", "SegmentCount", ms.segmentCount);
    ms.matchDeltaBarWidth = ReadBool("MiniSector", "MatchDeltaBarWidth", ms.matchDeltaBarWidth);
    ms.widthFraction = ReadFloat("MiniSector", "WidthFraction", ms.widthFraction);
    ms.centerXFraction = ReadFloat("MiniSector", "CenterXFraction", ms.centerXFraction);
    ms.centerYFraction = ReadFloat("MiniSector", "CenterYFraction", ms.centerYFraction);
    ms.scale = ReadFloat("MiniSector", "Scale", ms.scale);
    ms.barHeightPx = ReadFloat("MiniSector", "BarHeightPixels", ms.barHeightPx);
    ms.rowGapPx = ReadFloat("MiniSector", "RowGapPixels", ms.rowGapPx);
    ms.gapAboveDeltaBarPx = ReadFloat("MiniSector", "GapAboveDeltaBarPixels", ms.gapAboveDeltaBarPx);
    ms.useSessionReference = ReadBool("MiniSector", "UseSessionReference", ms.useSessionReference);
    ms.scaleStepKmh = ReadFloat("MiniSector", "ScaleStepKmh", ms.scaleStepKmh);
    ms.scaleStepCount = ReadInt("MiniSector", "ScaleStepCount", ms.scaleStepCount);
    ms.scalePercentile = ReadFloat("MiniSector", "ScalePercentile", ms.scalePercentile);
    ms.showScaleLabels = ReadBool("MiniSector", "ShowScaleLabels", ms.showScaleLabels);
    ms.scaleLabelTextHeightPx = ReadFloat("MiniSector", "ScaleLabelTextHeightPixels", ms.scaleLabelTextHeightPx);
    ms.scaleLabelGapPx = ReadFloat("MiniSector", "ScaleLabelGapPixels", ms.scaleLabelGapPx);
    ms.fasterR = ReadFloat("MiniSector", "FasterColorR", ms.fasterR);
    ms.fasterG = ReadFloat("MiniSector", "FasterColorG", ms.fasterG);
    ms.fasterB = ReadFloat("MiniSector", "FasterColorB", ms.fasterB);
    ms.slowerR = ReadFloat("MiniSector", "SlowerColorR", ms.slowerR);
    ms.slowerG = ReadFloat("MiniSector", "SlowerColorG", ms.slowerG);
    ms.slowerB = ReadFloat("MiniSector", "SlowerColorB", ms.slowerB);
    ms.useTriColorScale = ReadBool("MiniSector", "UseTriColorScale", ms.useTriColorScale);
    ms.triMidR = ReadFloat("MiniSector", "TriMidColorR", ms.triMidR);
    ms.triMidG = ReadFloat("MiniSector", "TriMidColorG", ms.triMidG);
    ms.triMidB = ReadFloat("MiniSector", "TriMidColorB", ms.triMidB);
    ms.neutralR = ReadFloat("MiniSector", "NeutralColorR", ms.neutralR);
    ms.neutralG = ReadFloat("MiniSector", "NeutralColorG", ms.neutralG);
    ms.neutralB = ReadFloat("MiniSector", "NeutralColorB", ms.neutralB);
    ms.midFasterR = ReadFloat("MiniSector", "MidFasterColorR", ms.midFasterR);
    ms.midFasterG = ReadFloat("MiniSector", "MidFasterColorG", ms.midFasterG);
    ms.midFasterB = ReadFloat("MiniSector", "MidFasterColorB", ms.midFasterB);
    ms.midSlowerR = ReadFloat("MiniSector", "MidSlowerColorR", ms.midSlowerR);
    ms.midSlowerG = ReadFloat("MiniSector", "MidSlowerColorG", ms.midSlowerG);
    ms.midSlowerB = ReadFloat("MiniSector", "MidSlowerColorB", ms.midSlowerB);
    ms.neutralDeadbandKmh = ReadFloat("MiniSector", "NeutralDeadbandKmh", ms.neutralDeadbandKmh);
    ms.alpha = ReadFloat("MiniSector", "Alpha", ms.alpha);
    ms.accelLineEnabled = ReadBool("MiniSector", "AccelLineEnabled", ms.accelLineEnabled);
    ms.accelLineHeightPx = ReadFloat("MiniSector", "AccelLineHeightPixels", ms.accelLineHeightPx);
    ms.accelLineBrakeThreshold = ReadFloat("MiniSector", "AccelLineBrakeThreshold", ms.accelLineBrakeThreshold);
    ms.accelLineThrottleThreshold = ReadFloat("MiniSector", "AccelLineThrottleThreshold", ms.accelLineThrottleThreshold);
    ms.accelLineAccelR = ReadFloat("MiniSector", "AccelLineAccelColorR", ms.accelLineAccelR);
    ms.accelLineAccelG = ReadFloat("MiniSector", "AccelLineAccelColorG", ms.accelLineAccelG);
    ms.accelLineAccelB = ReadFloat("MiniSector", "AccelLineAccelColorB", ms.accelLineAccelB);
    ms.accelLineBrakeR = ReadFloat("MiniSector", "AccelLineBrakeColorR", ms.accelLineBrakeR);
    ms.accelLineBrakeG = ReadFloat("MiniSector", "AccelLineBrakeColorG", ms.accelLineBrakeG);
    ms.accelLineBrakeB = ReadFloat("MiniSector", "AccelLineBrakeColorB", ms.accelLineBrakeB);
    ms.accelLineAlpha = ReadFloat("MiniSector", "AccelLineAlpha", ms.accelLineAlpha);
    ms.bgEnabled = ReadBool("MiniSector", "BackgroundEnabled", ms.bgEnabled);
    ms.bgPaddingPx = ReadFloat("MiniSector", "BackgroundPaddingPixels", ms.bgPaddingPx);
    ms.bgCornerRadiusPx = ReadFloat("MiniSector", "BackgroundCornerRadiusPixels", ms.bgCornerRadiusPx);
    ms.bgR = ReadFloat("MiniSector", "BackgroundR", ms.bgR);
    ms.bgG = ReadFloat("MiniSector", "BackgroundG", ms.bgG);
    ms.bgB = ReadFloat("MiniSector", "BackgroundB", ms.bgB);
    ms.bgA = ReadFloat("MiniSector", "BackgroundA", ms.bgA);

    PerfUpdateConfig& pu = g_perfUpdateConfig;
    pu.updateHz = ReadFloat("Performance", "UpdateHz", pu.updateHz);
    if (pu.updateHz < 0.2f) pu.updateHz = 0.2f;
    if (pu.updateHz > 30.0f) pu.updateHz = 30.0f;

    FrameLatencyConfig& fl = g_frameLatencyConfig;
    fl.enabled = ReadBool("FrameLatency", "Enabled", fl.enabled);
    fl.textHeightPx = ReadFloat("FrameLatency", "TextHeightPixels", fl.textHeightPx);
    fl.cpuGpuPosXFraction = ReadFloat("FrameLatency", "CpuGpuPosXFraction", fl.cpuGpuPosXFraction);
    fl.cpuGpuPosYFraction = ReadFloat("FrameLatency", "CpuGpuPosYFraction", fl.cpuGpuPosYFraction);
    fl.scale = ReadFloat("FrameLatency", "Scale", fl.scale);
    fl.alignBottomRightCorner = ReadBool("FrameLatency", "AlignBottomRightCorner", fl.alignBottomRightCorner);
    fl.alignTop = ReadBool("FrameLatency", "AlignTop", fl.alignTop);
    fl.alignRightToPedalTrace = ReadBool("FrameLatency", "AlignRightToPedalTrace", fl.alignRightToPedalTrace);
    fl.colorR = ReadFloat("FrameLatency", "ColorR", fl.colorR);
    fl.colorG = ReadFloat("FrameLatency", "ColorG", fl.colorG);
    fl.colorB = ReadFloat("FrameLatency", "ColorB", fl.colorB);
    fl.showFps = ReadBool("FrameLatency", "ShowFps", fl.showFps);
    fl.showInputLagEstimate = ReadBool("FrameLatency", "ShowInputLagEstimate", fl.showInputLagEstimate);
    fl.showFrameTime = ReadBool("FrameLatency", "ShowFrameTime", fl.showFrameTime);
    fl.showDisplayLatency = ReadBool("FrameLatency", "ShowDisplayLatency", fl.showDisplayLatency);
    fl.showCpuFrameTime = ReadBool("FrameLatency", "ShowCpuFrameTime", fl.showCpuFrameTime);
    fl.showProcessRam = ReadBool("FrameLatency", "ShowProcessRam", fl.showProcessRam);
    fl.showVram = ReadBool("FrameLatency", "ShowVram", fl.showVram);
    fl.showFpsRange = ReadBool("FrameLatency", "ShowFpsRange", fl.showFpsRange);
    fl.showGpuFrameTime = ReadBool("FrameLatency", "ShowGpuFrameTime", fl.showGpuFrameTime);
    fl.showGpuPower = ReadBool("FrameLatency", "ShowGpuPower", fl.showGpuPower);
    fl.bgEnabled = ReadBool("FrameLatency", "BackgroundEnabled", fl.bgEnabled);
    fl.bgPaddingPx = ReadFloat("FrameLatency", "BackgroundPaddingPixels", fl.bgPaddingPx);
    fl.bgCornerRadiusPx = ReadFloat("FrameLatency", "BackgroundCornerRadiusPixels", fl.bgCornerRadiusPx);
    fl.bgR = ReadFloat("FrameLatency", "BackgroundR", fl.bgR);
    fl.bgG = ReadFloat("FrameLatency", "BackgroundG", fl.bgG);
    fl.bgB = ReadFloat("FrameLatency", "BackgroundB", fl.bgB);
    fl.bgA = ReadFloat("FrameLatency", "BackgroundA", fl.bgA);

    GearDisplayConfig& gd = g_gearDisplayConfig;
    gd.enabled = ReadBool("GearDisplay", "Enabled", gd.enabled);
    gd.centerXFraction = ReadFloat("GearDisplay", "CenterXFraction", gd.centerXFraction);
    gd.centerYFraction = ReadFloat("GearDisplay", "CenterYFraction", gd.centerYFraction);
    gd.scale = ReadFloat("GearDisplay", "Scale", gd.scale);
    gd.slipFeedbackEnabled = ReadBool("GearDisplay", "SlipFeedbackEnabled", gd.slipFeedbackEnabled);
    gd.lockSlipThreshold = ReadFloat("GearDisplay", "LockSlipThreshold", gd.lockSlipThreshold);
    gd.spinSlipThreshold = ReadFloat("GearDisplay", "SpinSlipThreshold", gd.spinSlipThreshold);
    gd.slipFeedbackBrakeInput = ReadFloat("GearDisplay", "SlipFeedbackBrakeInput", gd.slipFeedbackBrakeInput);
    gd.slipFeedbackDwellSeconds = ReadFloat("GearDisplay", "SlipFeedbackDwellSeconds", gd.slipFeedbackDwellSeconds);
    gd.slipFeedbackHoldSeconds = ReadFloat("GearDisplay", "SlipFeedbackHoldSeconds", gd.slipFeedbackHoldSeconds);
    gd.lockSlipR = ReadFloat("GearDisplay", "LockSlipColorR", gd.lockSlipR);
    gd.lockSlipG = ReadFloat("GearDisplay", "LockSlipColorG", gd.lockSlipG);
    gd.lockSlipB = ReadFloat("GearDisplay", "LockSlipColorB", gd.lockSlipB);
    gd.spinSlipR = ReadFloat("GearDisplay", "SpinSlipColorR", gd.spinSlipR);
    gd.spinSlipG = ReadFloat("GearDisplay", "SpinSlipColorG", gd.spinSlipG);
    gd.spinSlipB = ReadFloat("GearDisplay", "SpinSlipColorB", gd.spinSlipB);
    gd.absTcBoxEnabled = ReadBool("GearDisplay", "AbsTcBoxEnabled", gd.absTcBoxEnabled);
    gd.showAbs = ReadBool("GearDisplay", "ShowAbs", gd.showAbs);
    gd.showTc = ReadBool("GearDisplay", "ShowTc", gd.showTc);
    gd.absTcBoxGapPx = ReadFloat("GearDisplay", "AbsTcBoxGapPixels", gd.absTcBoxGapPx);
    gd.tyreLockupBoxEnabled = ReadBool("GearDisplay", "TyreLockupBoxEnabled", gd.tyreLockupBoxEnabled);
    gd.tyreLockupPosXFraction = ReadFloat("GearDisplay", "TyreLockupPosXFraction", gd.tyreLockupPosXFraction);
    gd.tyreLockupPosYFraction = ReadFloat("GearDisplay", "TyreLockupPosYFraction", gd.tyreLockupPosYFraction);
    gd.tyreLockupScale = ReadFloat("GearDisplay", "TyreLockupScale", gd.tyreLockupScale);
    gd.tyreLockupFullScale = ReadFloat("GearDisplay", "TyreLockupFullScale", gd.tyreLockupFullScale);
    gd.tyreLockupFadeSeconds = ReadFloat("GearDisplay", "TyreLockupFadeSeconds", gd.tyreLockupFadeSeconds);
    gd.tyreLockupR = ReadFloat("GearDisplay", "TyreLockupColorR", gd.tyreLockupR);
    gd.tyreLockupG = ReadFloat("GearDisplay", "TyreLockupColorG", gd.tyreLockupG);
    gd.tyreLockupB = ReadFloat("GearDisplay", "TyreLockupColorB", gd.tyreLockupB);
    gd.gearTextHeightPx = ReadFloat("GearDisplay", "GearTextHeightPixels", gd.gearTextHeightPx);
    gd.normalR = ReadFloat("GearDisplay", "NormalColorR", gd.normalR);
    gd.normalG = ReadFloat("GearDisplay", "NormalColorG", gd.normalG);
    gd.normalB = ReadFloat("GearDisplay", "NormalColorB", gd.normalB);
    gd.shiftRpmFraction = ReadFloat("GearDisplay", "ShiftRpmFraction", gd.shiftRpmFraction);
    gd.shiftR = ReadFloat("GearDisplay", "ShiftColorR", gd.shiftR);
    gd.shiftG = ReadFloat("GearDisplay", "ShiftColorG", gd.shiftG);
    gd.shiftB = ReadFloat("GearDisplay", "ShiftColorB", gd.shiftB);
    gd.revLimiterRpmFraction = ReadFloat("GearDisplay", "RevLimiterRpmFraction", gd.revLimiterRpmFraction);
    gd.revLimiterR = ReadFloat("GearDisplay", "RevLimiterColorR", gd.revLimiterR);
    gd.revLimiterG = ReadFloat("GearDisplay", "RevLimiterColorG", gd.revLimiterG);
    gd.revLimiterB = ReadFloat("GearDisplay", "RevLimiterColorB", gd.revLimiterB);
    gd.trueLimiterWarnFraction = ReadFloat("GearDisplay", "TrueLimiterWarnFraction", gd.trueLimiterWarnFraction);
    gd.trueLimiterR = ReadFloat("GearDisplay", "TrueLimiterColorR", gd.trueLimiterR);
    gd.trueLimiterG = ReadFloat("GearDisplay", "TrueLimiterColorG", gd.trueLimiterG);
    gd.trueLimiterB = ReadFloat("GearDisplay", "TrueLimiterColorB", gd.trueLimiterB);
    gd.neutralWarnEnabled = ReadBool("GearDisplay", "NeutralWarnEnabled", gd.neutralWarnEnabled);
    gd.neutralWarnR = ReadFloat("GearDisplay", "NeutralWarnColorR", gd.neutralWarnR);
    gd.neutralWarnG = ReadFloat("GearDisplay", "NeutralWarnColorG", gd.neutralWarnG);
    gd.neutralWarnB = ReadFloat("GearDisplay", "NeutralWarnColorB", gd.neutralWarnB);
    gd.firstGearWarnEnabled = ReadBool("GearDisplay", "FirstGearWarnEnabled", gd.firstGearWarnEnabled);
    gd.firstGearWarnR = ReadFloat("GearDisplay", "FirstGearWarnColorR", gd.firstGearWarnR);
    gd.firstGearWarnG = ReadFloat("GearDisplay", "FirstGearWarnColorG", gd.firstGearWarnG);
    gd.firstGearWarnB = ReadFloat("GearDisplay", "FirstGearWarnColorB", gd.firstGearWarnB);
    gd.shiftReactionAdaptiveEnabled = ReadBool("GearDisplay", "ShiftReactionAdaptiveEnabled", gd.shiftReactionAdaptiveEnabled);
    gd.shiftReactionDefaultSeconds = ReadFloat("GearDisplay", "ShiftReactionDefaultSeconds", gd.shiftReactionDefaultSeconds);
    gd.shiftReactionMinSeconds = ReadFloat("GearDisplay", "ShiftReactionMinSeconds", gd.shiftReactionMinSeconds);
    gd.shiftReactionMaxSeconds = ReadFloat("GearDisplay", "ShiftReactionMaxSeconds", gd.shiftReactionMaxSeconds);
    gd.shiftReactionTimeoutSeconds = ReadFloat("GearDisplay", "ShiftReactionTimeoutSeconds", gd.shiftReactionTimeoutSeconds);
    gd.shiftCueMinThrottle = ReadFloat("GearDisplay", "ShiftCueMinThrottle", gd.shiftCueMinThrottle);
    gd.shiftLearnOutlierBand = ReadFloat("GearDisplay", "ShiftLearnOutlierBand", gd.shiftLearnOutlierBand);
    gd.gearLearnMinThrottle = ReadFloat("GearDisplay", "GearLearnMinThrottle", gd.gearLearnMinThrottle);
    gd.blinkPeriodSeconds = ReadFloat("GearDisplay", "BlinkPeriodSeconds", gd.blinkPeriodSeconds);
    gd.backgroundEnabled = ReadBool("GearDisplay", "BackgroundEnabled", gd.backgroundEnabled);
    gd.bgPaddingPx = ReadFloat("GearDisplay", "BackgroundPaddingPixels", gd.bgPaddingPx);
    gd.bgCornerRadiusPx = ReadFloat("GearDisplay", "BackgroundCornerRadiusPixels", gd.bgCornerRadiusPx);
    gd.gapPx = ReadFloat("GearDisplay", "GapPixels", gd.gapPx);
    gd.bgR = ReadFloat("GearDisplay", "BackgroundR", gd.bgR);
    gd.bgG = ReadFloat("GearDisplay", "BackgroundG", gd.bgG);
    gd.bgB = ReadFloat("GearDisplay", "BackgroundB", gd.bgB);
    gd.bgA = ReadFloat("GearDisplay", "BackgroundA", gd.bgA);
    gd.borderEnabled = ReadBool("GearDisplay", "BorderEnabled", gd.borderEnabled);
    gd.borderThicknessPx = ReadFloat("GearDisplay", "BorderThicknessPixels", gd.borderThicknessPx);
    gd.borderR = ReadFloat("GearDisplay", "BorderColorR", gd.borderR);
    gd.borderG = ReadFloat("GearDisplay", "BorderColorG", gd.borderG);
    gd.borderB = ReadFloat("GearDisplay", "BorderColorB", gd.borderB);
    gd.speedEnabled = ReadBool("GearDisplay", "SpeedEnabled", gd.speedEnabled);
    gd.speedUnitsKmh = ReadBool("GearDisplay", "SpeedUnitsKmh", gd.speedUnitsKmh);
    gd.speedTextHeightPx = ReadFloat("GearDisplay", "SpeedTextHeightPixels", gd.speedTextHeightPx);
    gd.speedR = ReadFloat("GearDisplay", "SpeedColorR", gd.speedR);
    gd.speedG = ReadFloat("GearDisplay", "SpeedColorG", gd.speedG);
    gd.speedB = ReadFloat("GearDisplay", "SpeedColorB", gd.speedB);
    gd.dividerR = ReadFloat("GearDisplay", "DividerColorR", gd.dividerR);
    gd.dividerG = ReadFloat("GearDisplay", "DividerColorG", gd.dividerG);
    gd.dividerB = ReadFloat("GearDisplay", "DividerColorB", gd.dividerB);
    gd.absTcFlashR = ReadFloat("GearDisplay", "AbsTcFlashColorR", gd.absTcFlashR);
    gd.absTcFlashG = ReadFloat("GearDisplay", "AbsTcFlashColorG", gd.absTcFlashG);
    gd.absTcFlashB = ReadFloat("GearDisplay", "AbsTcFlashColorB", gd.absTcFlashB);
    gd.absTcThreshold = ReadFloat("GearDisplay", "AbsTcThreshold", gd.absTcThreshold);
    gd.idleFadeSeconds = ReadFloat("GearDisplay", "IdleFadeSeconds", gd.idleFadeSeconds);
    gd.showShiftStats = ReadBool("GearDisplay", "ShowShiftStats", gd.showShiftStats);
    gd.shiftStatsTextHeightPx = ReadFloat("GearDisplay", "ShiftStatsTextHeightPixels", gd.shiftStatsTextHeightPx);
    gd.shiftStatsGapPx = ReadFloat("GearDisplay", "ShiftStatsGapPixels", gd.shiftStatsGapPx);
    gd.shiftStatsR = ReadFloat("GearDisplay", "ShiftStatsColorR", gd.shiftStatsR);
    gd.shiftStatsG = ReadFloat("GearDisplay", "ShiftStatsColorG", gd.shiftStatsG);
    gd.shiftStatsB = ReadFloat("GearDisplay", "ShiftStatsColorB", gd.shiftStatsB);
    gd.shiftStatsPosXFraction = ReadFloat("GearDisplay", "ShiftStatsPosXFraction", gd.shiftStatsPosXFraction);
    gd.shiftStatsPosYFraction = ReadFloat("GearDisplay", "ShiftStatsPosYFraction", gd.shiftStatsPosYFraction);
    gd.shiftStatsScale = ReadFloat("GearDisplay", "ShiftStatsScale", gd.shiftStatsScale);
    gd.shiftStatsBgEnabled = ReadBool("GearDisplay", "ShiftStatsBgEnabled", gd.shiftStatsBgEnabled);
    gd.shiftStatsBgPaddingPx = ReadFloat("GearDisplay", "ShiftStatsBgPaddingPixels", gd.shiftStatsBgPaddingPx);
    gd.shiftStatsBgCornerRadiusPx = ReadFloat("GearDisplay", "ShiftStatsBgCornerRadiusPixels", gd.shiftStatsBgCornerRadiusPx);
    gd.shiftStatsBgR = ReadFloat("GearDisplay", "ShiftStatsBgColorR", gd.shiftStatsBgR);
    gd.shiftStatsBgG = ReadFloat("GearDisplay", "ShiftStatsBgColorG", gd.shiftStatsBgG);
    gd.shiftStatsBgB = ReadFloat("GearDisplay", "ShiftStatsBgColorB", gd.shiftStatsBgB);
    gd.shiftStatsBgA = ReadFloat("GearDisplay", "ShiftStatsBgColorA", gd.shiftStatsBgA);

    PitLimiterConfig& pl = g_pitLimiterConfig;
    pl.enabled = ReadBool("PitLimiter", "Enabled", pl.enabled);
    pl.offsetYFraction = ReadFloat("PitLimiter", "OffsetYFraction", pl.offsetYFraction);
    pl.textHeightPx = ReadFloat("PitLimiter", "TextHeightPixels", pl.textHeightPx);
    pl.paddingPx = ReadFloat("PitLimiter", "PaddingPixels", pl.paddingPx);
    pl.textR = ReadFloat("PitLimiter", "TextColorR", pl.textR);
    pl.textG = ReadFloat("PitLimiter", "TextColorG", pl.textG);
    pl.textB = ReadFloat("PitLimiter", "TextColorB", pl.textB);
    pl.speedLimitKmh = ReadFloat("PitLimiter", "SpeedLimitKmh", pl.speedLimitKmh);
    pl.fillR = ReadFloat("PitLimiter", "FillColorR", pl.fillR);
    pl.fillG = ReadFloat("PitLimiter", "FillColorG", pl.fillG);
    pl.fillB = ReadFloat("PitLimiter", "FillColorB", pl.fillB);
    pl.midR = ReadFloat("PitLimiter", "MidColorR", pl.midR);
    pl.midG = ReadFloat("PitLimiter", "MidColorG", pl.midG);
    pl.midB = ReadFloat("PitLimiter", "MidColorB", pl.midB);
    pl.overLimitR = ReadFloat("PitLimiter", "OverLimitColorR", pl.overLimitR);
    pl.overLimitG = ReadFloat("PitLimiter", "OverLimitColorG", pl.overLimitG);
    pl.overLimitB = ReadFloat("PitLimiter", "OverLimitColorB", pl.overLimitB);
    pl.overLimitFarKmh = ReadFloat("PitLimiter", "OverLimitFarKmh", pl.overLimitFarKmh);
    pl.overLimitFlashPeriodSeconds = ReadFloat("PitLimiter", "OverLimitFlashPeriodSeconds", pl.overLimitFlashPeriodSeconds);
    pl.showLimitAndDelta = ReadBool("PitLimiter", "ShowLimitAndDelta", pl.showLimitAndDelta);
    pl.infoTextHeightPx = ReadFloat("PitLimiter", "InfoTextHeightPixels", pl.infoTextHeightPx);
    pl.deltaTextHeightPx = ReadFloat("PitLimiter", "DeltaTextHeightPixels", pl.deltaTextHeightPx);
    pl.engageBeepEnabled = ReadBool("PitLimiter", "EngageBeepEnabled", pl.engageBeepEnabled);
    pl.engageBeepFrequencyHz = ReadInt("PitLimiter", "EngageBeepFrequencyHz", pl.engageBeepFrequencyHz);
    pl.engageBeepDurationMs = ReadInt("PitLimiter", "EngageBeepDurationMs", pl.engageBeepDurationMs);
    pl.engageWarningEnabled = ReadBool("PitLimiter", "EngageWarningEnabled", pl.engageWarningEnabled);
    pl.engageWarningMaxDistanceM = ReadFloat("PitLimiter", "EngageWarningMaxDistanceMeters", pl.engageWarningMaxDistanceM);
    pl.engageWarningTextHeightPx = ReadFloat("PitLimiter", "EngageWarningTextHeightPixels", pl.engageWarningTextHeightPx);
    pl.barXFraction = ReadFloat("PitLimiter", "BarXFraction", pl.barXFraction);
    pl.barYFraction = ReadFloat("PitLimiter", "BarYFraction", pl.barYFraction);
    pl.barScale = ReadFloat("PitLimiter", "BarScale", pl.barScale);
    pl.engageWarningXFraction = ReadFloat("PitLimiter", "EngageWarningXFraction", pl.engageWarningXFraction);
    pl.engageWarningYFraction = ReadFloat("PitLimiter", "EngageWarningYFraction", pl.engageWarningYFraction);
    pl.engageWarningBlinkPeriodSeconds = ReadFloat("PitLimiter", "EngageWarningBlinkPeriodSeconds", pl.engageWarningBlinkPeriodSeconds);
    pl.engageWarningR = ReadFloat("PitLimiter", "EngageWarningColorR", pl.engageWarningR);
    pl.engageWarningG = ReadFloat("PitLimiter", "EngageWarningColorG", pl.engageWarningG);
    pl.engageWarningB = ReadFloat("PitLimiter", "EngageWarningColorB", pl.engageWarningB);
    pl.entryOffsetMeters = ReadFloat("PitLimiter", "EntryOffsetMeters", pl.entryOffsetMeters);

    TyreTempsConfig& tt = g_tyreTempsConfig;
    tt.enabled = ReadBool("TyreTemps", "Enabled", tt.enabled);
    tt.posXFraction = ReadFloat("TyreTemps", "PosXFraction", tt.posXFraction);
    tt.posYFraction = ReadFloat("TyreTemps", "PosYFraction", tt.posYFraction);
    tt.scale = ReadFloat("TyreTemps", "Scale", tt.scale);
    tt.textHeightPx = ReadFloat("TyreTemps", "TextHeightPixels", tt.textHeightPx);
    tt.labelHeightPx = ReadFloat("TyreTemps", "LabelHeightPixels", tt.labelHeightPx);
    tt.rowGapPx = ReadFloat("TyreTemps", "RowGapPixels", tt.rowGapPx);
    tt.columnGapPx = ReadFloat("TyreTemps", "ColumnGapPixels", tt.columnGapPx);
    tt.swapInsideOutside = ReadBool("TyreTemps", "SwapInsideOutside", tt.swapInsideOutside);
    tt.cornerEnterG = ReadFloat("TyreTemps", "CornerEnterG", tt.cornerEnterG);
    tt.cornerExitG = ReadFloat("TyreTemps", "CornerExitG", tt.cornerExitG);
    tt.enterHoldSeconds = ReadFloat("TyreTemps", "EnterHoldSeconds", tt.enterHoldSeconds);
    tt.exitHoldSeconds = ReadFloat("TyreTemps", "ExitHoldSeconds", tt.exitHoldSeconds);
    tt.tailSeconds = ReadFloat("TyreTemps", "TailSeconds", tt.tailSeconds);
    tt.minCornerSeconds = ReadFloat("TyreTemps", "MinCornerSeconds", tt.minCornerSeconds);
    tt.spreadGoodMinC = ReadFloat("TyreTemps", "SpreadGoodMinC", tt.spreadGoodMinC);
    tt.spreadGoodMaxC = ReadFloat("TyreTemps", "SpreadGoodMaxC", tt.spreadGoodMaxC);
    tt.barMinRangeC = ReadFloat("TyreTemps", "BarMinRangeC", tt.barMinRangeC);
    tt.barColdC = ReadFloat("TyreTemps", "BarColdC", tt.barColdC);
    tt.barHotC = ReadFloat("TyreTemps", "BarHotC", tt.barHotC);

    FfbGraphConfig& fg = g_ffbGraphConfig;
    fg.enabled = ReadBool("FfbGraph", "Enabled", fg.enabled);
    fg.posXFraction = ReadFloat("FfbGraph", "PosXFraction", fg.posXFraction);
    fg.posYFraction = ReadFloat("FfbGraph", "PosYFraction", fg.posYFraction);
    fg.scale = ReadFloat("FfbGraph", "Scale", fg.scale);
    fg.widthPx = ReadFloat("FfbGraph", "WidthPixels", fg.widthPx);
    fg.heightPx = ReadFloat("FfbGraph", "HeightPixels", fg.heightPx);
    fg.textHeightPx = ReadFloat("FfbGraph", "TextHeightPixels", fg.textHeightPx);
    fg.windowSeconds = ReadFloat("FfbGraph", "WindowSeconds", fg.windowSeconds);
    if (fg.windowSeconds < 1.0f) fg.windowSeconds = 1.0f;
    if (fg.windowSeconds > 30.0f) fg.windowSeconds = 30.0f;
    fg.realtime = ReadBool("FfbGraph", "Realtime", fg.realtime);
    fg.showLapAverage = ReadBool("FfbGraph", "ShowLapAverage", fg.showLapAverage);
    fg.clipLevel = ReadFloat("FfbGraph", "ClipLevel", fg.clipLevel);
    fg.clipFadeSeconds = ReadFloat("FfbGraph", "ClipFadeSeconds", fg.clipFadeSeconds);
    fg.minPercent = ReadFloat("FfbGraph", "MinPercent", fg.minPercent);
    fg.maxPercent = ReadFloat("FfbGraph", "MaxPercent", fg.maxPercent);
    fg.bgA = ReadFloat("FfbGraph", "BackgroundAlpha", fg.bgA);

    CoverBoxConfig& cb = g_coverBoxConfig;
    cb.count = ReadInt("CoverBox", "Count", cb.count);
    if (cb.count < 1) cb.count = 1;
    if (cb.count > CoverBoxConfig::kMaxBoxes) cb.count = CoverBoxConfig::kMaxBoxes;
    for (int i = 0; i < CoverBoxConfig::kMaxBoxes; ++i)
    {
        char k[32];
        sprintf_s(k, "Box%dPosXFraction", i + 1); cb.posXFraction[i] = ReadFloat("CoverBox", k, cb.posXFraction[i]);
        sprintf_s(k, "Box%dPosYFraction", i + 1); cb.posYFraction[i] = ReadFloat("CoverBox", k, cb.posYFraction[i]);
        sprintf_s(k, "Box%dWidthPixels", i + 1);  cb.widthPx[i] = ReadFloat("CoverBox", k, cb.widthPx[i]);
        sprintf_s(k, "Box%dHeightPixels", i + 1); cb.heightPx[i] = ReadFloat("CoverBox", k, cb.heightPx[i]);
    }
    cb.cornerRadiusPx = ReadFloat("CoverBox", "CornerRadiusPixels", cb.cornerRadiusPx);
    cb.grey = ReadFloat("CoverBox", "Grey", cb.grey);
    if (cb.grey < 0.0f) cb.grey = 0.0f;
    if (cb.grey > 1.0f) cb.grey = 1.0f;
    cb.a = ReadFloat("CoverBox", "Alpha", cb.a);
    if (cb.a < 0.0f) cb.a = 0.0f;
    if (cb.a > 1.0f) cb.a = 1.0f;

    TrackMapConfig& tm = g_trackMapConfig;
    tm.enabled = ReadBool("TrackMap", "Enabled", tm.enabled);
    tm.posXFraction = ReadFloat("TrackMap", "PosXFraction", tm.posXFraction);
    tm.posYFraction = ReadFloat("TrackMap", "PosYFraction", tm.posYFraction);
    tm.scale = ReadFloat("TrackMap", "Scale", tm.scale);
    tm.sizePx = ReadFloat("TrackMap", "SizePixels", tm.sizePx);
    tm.rotationDegrees = ReadFloat("TrackMap", "RotationDegrees", tm.rotationDegrees);
    tm.mirror = ReadBool("TrackMap", "Mirror", tm.mirror);
    tm.recordMissing = ReadBool("TrackMap", "RecordMissing", tm.recordMissing);
    tm.showSectors = ReadBool("TrackMap", "ShowSectors", tm.showSectors);
    tm.showNorth = ReadBool("TrackMap", "ShowNorth", tm.showNorth);
    tm.deltaColourMode = ReadInt("TrackMap", "DeltaColourMode", tm.deltaColourMode);
    if (tm.deltaColourMode < 0 || tm.deltaColourMode > 3) tm.deltaColourMode = 0;
    tm.deltaPrevButton = ReadInt("TrackMap", "DeltaPrevButton", tm.deltaPrevButton);
    tm.deltaNextButton = ReadInt("TrackMap", "DeltaNextButton", tm.deltaNextButton);
    tm.deltaSectorCount = ReadInt("TrackMap", "DeltaSectorCount", tm.deltaSectorCount);
    if (tm.deltaSectorCount < TrackMapConfig::kMinDeltaSectors) tm.deltaSectorCount = TrackMapConfig::kMinDeltaSectors;
    if (tm.deltaSectorCount > TrackMapConfig::kMaxDeltaSectors) tm.deltaSectorCount = TrackMapConfig::kMaxDeltaSectors;
    tm.deltaFullSeconds = ReadFloat("TrackMap", "DeltaFullSeconds", tm.deltaFullSeconds);
    if (tm.deltaFullSeconds < 0.005f) tm.deltaFullSeconds = 0.005f;
    tm.deltaAheadStrength = ReadFloat("TrackMap", "DeltaAheadStrength", tm.deltaAheadStrength);
    tm.showLabels = ReadBool("TrackMap", "ShowLabels", tm.showLabels);
    tm.playerOwnColor = ReadBool("TrackMap", "PlayerOwnColor", tm.playerOwnColor);
    tm.lateralEnabled = ReadBool("TrackMap", "LateralEnabled", tm.lateralEnabled);
    tm.lateralFlip = ReadBool("TrackMap", "LateralFlip", tm.lateralFlip);
    tm.bgA = ReadFloat("TrackMap", "BackgroundAlpha", tm.bgA);
    ReadString("TrackMap", "TinyPedalMapDir", tm.tinyPedalMapDir, sizeof(tm.tinyPedalMapDir));
    {
        // The loader appends the file name straight on, so it needs the slash.
        const size_t n = strlen(tm.tinyPedalMapDir);
        if (n > 0 && n + 1 < sizeof(tm.tinyPedalMapDir)
            && tm.tinyPedalMapDir[n - 1] != '\\' && tm.tinyPedalMapDir[n - 1] != '/')
            strcat_s(tm.tinyPedalMapDir, "\\");
    }


    LapDeltaConfig& ld = g_lapDeltaConfig;
    ld.enabled = ReadBool("LapDelta", "Enabled", ld.enabled);
    ld.centerXFraction = ReadFloat("LapDelta", "CenterXFraction", ld.centerXFraction);
    ld.centerYFraction = ReadFloat("LapDelta", "CenterYFraction", ld.centerYFraction);
    ld.scale = ReadFloat("LapDelta", "Scale", ld.scale);
    ld.drsScale = ReadFloat("LapDelta", "DrsScale", ld.drsScale);
    ld.bestScale = ReadFloat("LapDelta", "BestScale", ld.bestScale);
    ld.colorWindowSeconds = ReadFloat("LapDelta", "ColorWindowSeconds", ld.colorWindowSeconds);
    ld.colorDeadbandSeconds = ReadFloat("LapDelta", "ColorDeadbandSeconds", ld.colorDeadbandSeconds);
    ld.barWidthFraction = ReadFloat("LapDelta", "BarWidthFraction", ld.barWidthFraction);
    ld.barHeightPx = ReadFloat("LapDelta", "BarHeightPixels", ld.barHeightPx);
    ld.numberGapPx = ReadFloat("LapDelta", "NumberGapPixels", ld.numberGapPx);
    ld.textHeightPx = ReadFloat("LapDelta", "TextHeightPixels", ld.textHeightPx);
    ld.rowGapPx = ReadFloat("LapDelta", "RowGapPixels", ld.rowGapPx);
    ld.bestCenterXFraction = ReadFloat("LapDelta", "BestCenterXFraction", ld.bestCenterXFraction);
    ld.bestCenterYFraction = ReadFloat("LapDelta", "BestCenterYFraction", ld.bestCenterYFraction);
    ld.drsCenterXFraction = ReadFloat("LapDelta", "DrsCenterXFraction", ld.drsCenterXFraction);
    ld.drsCenterYFraction = ReadFloat("LapDelta", "DrsCenterYFraction", ld.drsCenterYFraction);
    ld.maxBarSeconds = ReadFloat("LapDelta", "MaxBarSeconds", ld.maxBarSeconds);
    ld.autoBarWindowEnabled = ReadBool("LapDelta", "AutoBarWindowEnabled", ld.autoBarWindowEnabled);
    ld.barWindowMaxSeconds = ReadFloat("LapDelta", "BarWindowMaxSeconds", ld.barWindowMaxSeconds);
    ld.numberAheadR = ReadFloat("LapDelta", "NumberAheadColorR", ld.numberAheadR);
    ld.numberAheadG = ReadFloat("LapDelta", "NumberAheadColorG", ld.numberAheadG);
    ld.numberAheadB = ReadFloat("LapDelta", "NumberAheadColorB", ld.numberAheadB);
    ld.numberBehindR = ReadFloat("LapDelta", "NumberBehindColorR", ld.numberBehindR);
    ld.numberBehindG = ReadFloat("LapDelta", "NumberBehindColorG", ld.numberBehindG);
    ld.numberBehindB = ReadFloat("LapDelta", "NumberBehindColorB", ld.numberBehindB);
    ld.barNumberR = ReadFloat("LapDelta", "BarNumberColorR", ld.barNumberR);
    ld.barNumberG = ReadFloat("LapDelta", "BarNumberColorG", ld.barNumberG);
    ld.barNumberB = ReadFloat("LapDelta", "BarNumberColorB", ld.barNumberB);
    ld.barNumberOutlineEnabled = ReadBool("LapDelta", "BarNumberOutlineEnabled", ld.barNumberOutlineEnabled);
    ld.barNumberOutlineR = ReadFloat("LapDelta", "BarNumberOutlineColorR", ld.barNumberOutlineR);
    ld.barNumberOutlineG = ReadFloat("LapDelta", "BarNumberOutlineColorG", ld.barNumberOutlineG);
    ld.barNumberOutlineB = ReadFloat("LapDelta", "BarNumberOutlineColorB", ld.barNumberOutlineB);
    ld.barNumberOutlineThicknessPx = ReadFloat("LapDelta", "BarNumberOutlineThicknessPixels", ld.barNumberOutlineThicknessPx);
    ld.gainingR = ReadFloat("LapDelta", "GainingColorR", ld.gainingR);
    ld.gainingG = ReadFloat("LapDelta", "GainingColorG", ld.gainingG);
    ld.gainingB = ReadFloat("LapDelta", "GainingColorB", ld.gainingB);
    ld.losingR = ReadFloat("LapDelta", "LosingColorR", ld.losingR);
    ld.losingG = ReadFloat("LapDelta", "LosingColorG", ld.losingG);
    ld.losingB = ReadFloat("LapDelta", "LosingColorB", ld.losingB);
    ld.barColorFadeSeconds = ReadFloat("LapDelta", "BarColorFadeSeconds", ld.barColorFadeSeconds);
    ld.trendWindowSeconds = ReadFloat("LapDelta", "TrendWindowSeconds", ld.trendWindowSeconds);
    ld.freezeAfterLineSeconds = ReadFloat("LapDelta", "FreezeAfterLineSeconds", ld.freezeAfterLineSeconds);
    ld.bgEnabled = ReadBool("LapDelta", "BackgroundEnabled", ld.bgEnabled);
    ld.bgPaddingPx = ReadFloat("LapDelta", "BackgroundPaddingPixels", ld.bgPaddingPx);
    ld.bgCornerRadiusPx = ReadFloat("LapDelta", "BackgroundCornerRadiusPixels", ld.bgCornerRadiusPx);
    ld.bgR = ReadFloat("LapDelta", "BackgroundR", ld.bgR);
    ld.bgG = ReadFloat("LapDelta", "BackgroundG", ld.bgG);
    ld.bgB = ReadFloat("LapDelta", "BackgroundB", ld.bgB);
    ld.bgA = ReadFloat("LapDelta", "BackgroundA", ld.bgA);
    ld.drsIndicatorEnabled = ReadBool("LapDelta", "DrsIndicatorEnabled", ld.drsIndicatorEnabled);
    ld.drsIndicatorGapPx = ReadFloat("LapDelta", "DrsIndicatorGapPixels", ld.drsIndicatorGapPx);
    ld.drsIndicatorTextHeightPx = ReadFloat("LapDelta", "DrsIndicatorTextHeightPixels", ld.drsIndicatorTextHeightPx);
    ld.drsIndicatorPaddingPx = ReadFloat("LapDelta", "DrsIndicatorPaddingPixels", ld.drsIndicatorPaddingPx);
    ld.drsIndicatorBgCornerRadiusPx = ReadFloat("LapDelta", "DrsIndicatorBackgroundCornerRadiusPixels", ld.drsIndicatorBgCornerRadiusPx);
    ld.drsIndicatorBgR = ReadFloat("LapDelta", "DrsIndicatorBackgroundR", ld.drsIndicatorBgR);
    ld.drsIndicatorBgG = ReadFloat("LapDelta", "DrsIndicatorBackgroundG", ld.drsIndicatorBgG);
    ld.drsIndicatorBgB = ReadFloat("LapDelta", "DrsIndicatorBackgroundB", ld.drsIndicatorBgB);
    ld.drsIndicatorBgA = ReadFloat("LapDelta", "DrsIndicatorBackgroundA", ld.drsIndicatorBgA);
    ld.drsIndicatorTextR = ReadFloat("LapDelta", "DrsIndicatorTextColorR", ld.drsIndicatorTextR);
    ld.drsIndicatorTextG = ReadFloat("LapDelta", "DrsIndicatorTextColorG", ld.drsIndicatorTextG);
    ld.drsIndicatorTextB = ReadFloat("LapDelta", "DrsIndicatorTextColorB", ld.drsIndicatorTextB);
    ld.drsIndicatorFlashPeriodSeconds = ReadFloat("LapDelta", "DrsIndicatorFlashPeriodSeconds", ld.drsIndicatorFlashPeriodSeconds);
    ld.showValidityDebug = ReadBool("LapDelta", "ShowValidityDebug", ld.showValidityDebug);
    ld.validityDebugTextHeightPx = ReadFloat("LapDelta", "ValidityDebugTextHeightPixels", ld.validityDebugTextHeightPx);
    ld.validityDebugGapPx = ReadFloat("LapDelta", "ValidityDebugGapPixels", ld.validityDebugGapPx);

    RelativeTimingConfig& rt = g_relativeTimingConfig;
    rt.enabled = ReadBool("RelativeTiming", "Enabled", rt.enabled);
    rt.posXFraction = ReadFloat("RelativeTiming", "PosXFraction", rt.posXFraction);
    rt.posYFraction = ReadFloat("RelativeTiming", "PosYFraction", rt.posYFraction);
    rt.scale = ReadFloat("RelativeTiming", "Scale", rt.scale);
    rt.showTyre = ReadBool("RelativeTiming", "ShowTyre", rt.showTyre);
    rt.textHeightPx = ReadFloat("RelativeTiming", "TextHeightPixels", rt.textHeightPx);
    rt.rowGapPx = ReadFloat("RelativeTiming", "RowGapPixels", rt.rowGapPx);
    rt.columnGapPx = ReadFloat("RelativeTiming", "ColumnGapPixels", rt.columnGapPx);
    rt.colorR = ReadFloat("RelativeTiming", "ColorR", rt.colorR);
    rt.colorG = ReadFloat("RelativeTiming", "ColorG", rt.colorG);
    rt.colorB = ReadFloat("RelativeTiming", "ColorB", rt.colorB);
    rt.playerR = ReadFloat("RelativeTiming", "PlayerColorR", rt.playerR);
    rt.playerG = ReadFloat("RelativeTiming", "PlayerColorG", rt.playerG);
    rt.playerB = ReadFloat("RelativeTiming", "PlayerColorB", rt.playerB);
    rt.lapDownR = ReadFloat("RelativeTiming", "LapDownColorR", rt.lapDownR);
    rt.lapUpR = ReadFloat("RelativeTiming", "LapUpColorR", rt.lapUpR);
    rt.lapUpG = ReadFloat("RelativeTiming", "LapUpColorG", rt.lapUpG);
    rt.lapUpB = ReadFloat("RelativeTiming", "LapUpColorB", rt.lapUpB);
    rt.lapDownG = ReadFloat("RelativeTiming", "LapDownColorG", rt.lapDownG);
    rt.lapDownB = ReadFloat("RelativeTiming", "LapDownColorB", rt.lapDownB);
    rt.gainingR = ReadFloat("RelativeTiming", "GainingColorR", rt.gainingR);
    rt.gainingG = ReadFloat("RelativeTiming", "GainingColorG", rt.gainingG);
    rt.gainingB = ReadFloat("RelativeTiming", "GainingColorB", rt.gainingB);
    rt.losingR = ReadFloat("RelativeTiming", "LosingColorR", rt.losingR);
    rt.losingG = ReadFloat("RelativeTiming", "LosingColorG", rt.losingG);
    rt.losingB = ReadFloat("RelativeTiming", "LosingColorB", rt.losingB);
    rt.lapFasterR = ReadFloat("RelativeTiming", "LapFasterColorR", rt.lapFasterR);
    rt.lapFasterG = ReadFloat("RelativeTiming", "LapFasterColorG", rt.lapFasterG);
    rt.lapFasterB = ReadFloat("RelativeTiming", "LapFasterColorB", rt.lapFasterB);
    rt.lapSlowerR = ReadFloat("RelativeTiming", "LapSlowerColorR", rt.lapSlowerR);
    rt.lapSlowerG = ReadFloat("RelativeTiming", "LapSlowerColorG", rt.lapSlowerG);
    rt.lapSlowerB = ReadFloat("RelativeTiming", "LapSlowerColorB", rt.lapSlowerB);
    rt.visibleCars = ReadFloat("RelativeTiming", "VisibleCars", rt.visibleCars);
    rt.raceTrackOrder = ReadBool("RelativeTiming", "RaceTrackOrder", rt.raceTrackOrder);
    rt.trendWindowSeconds = ReadFloat("RelativeTiming", "TrendWindowSeconds", rt.trendWindowSeconds);
    rt.trackRelativeThresholdSeconds = ReadFloat("RelativeTiming", "TrackRelativeThresholdSeconds", rt.trackRelativeThresholdSeconds);
    rt.logSdkGapFields = ReadBool("RelativeTiming", "LogSdkGapFields", rt.logSdkGapFields);
    rt.logRaceGaps = ReadBool("RelativeTiming", "LogRaceGaps", rt.logRaceGaps);

    rt.positionGainR = ReadFloat("RelativeTiming", "PositionGainColorR", rt.positionGainR);
    rt.positionGainG = ReadFloat("RelativeTiming", "PositionGainColorG", rt.positionGainG);
    rt.positionGainB = ReadFloat("RelativeTiming", "PositionGainColorB", rt.positionGainB);
    rt.positionGainA = ReadFloat("RelativeTiming", "PositionGainAlpha", rt.positionGainA);
    rt.positionLossR = ReadFloat("RelativeTiming", "PositionLossColorR", rt.positionLossR);
    rt.positionLossG = ReadFloat("RelativeTiming", "PositionLossColorG", rt.positionLossG);
    rt.positionLossB = ReadFloat("RelativeTiming", "PositionLossColorB", rt.positionLossB);
    rt.positionLossA = ReadFloat("RelativeTiming", "PositionLossAlpha", rt.positionLossA);
    rt.positionChangeHoldSeconds = ReadFloat("RelativeTiming", "PositionChangeHoldSeconds", rt.positionChangeHoldSeconds);
    rt.positionChangeDebounceSeconds = ReadFloat("RelativeTiming", "PositionChangeDebounceSeconds", rt.positionChangeDebounceSeconds);
    rt.bgEnabled = ReadBool("RelativeTiming", "BackgroundEnabled", rt.bgEnabled);
    rt.bgPaddingPx = ReadFloat("RelativeTiming", "BackgroundPaddingPixels", rt.bgPaddingPx);
    rt.bgCornerRadiusPx = ReadFloat("RelativeTiming", "BackgroundCornerRadiusPixels", rt.bgCornerRadiusPx);
    rt.bgR = ReadFloat("RelativeTiming", "BackgroundR", rt.bgR);
    rt.bgG = ReadFloat("RelativeTiming", "BackgroundG", rt.bgG);
    rt.bgB = ReadFloat("RelativeTiming", "BackgroundB", rt.bgB);
    rt.bgA = ReadFloat("RelativeTiming", "BackgroundA", rt.bgA);

    FuelSaveConfig& fs = g_fuelSaveConfig;
    fs.enabled = ReadBool("FuelSave", "Enabled", fs.enabled);
    fs.stintMinutes = ReadFloat("FuelSave", "StintMinutes", fs.stintMinutes);
    fs.reserveLitres = ReadFloat("FuelSave", "ReserveLitres", fs.reserveLitres);
    fs.barWindowLitres = ReadFloat("FuelSave", "BarWindowLitres", fs.barWindowLitres);
    fs.centerXFraction = ReadFloat("FuelSave", "CenterXFraction", fs.centerXFraction);
    fs.centerYFraction = ReadFloat("FuelSave", "CenterYFraction", fs.centerYFraction);
    fs.scale = ReadFloat("FuelSave", "Scale", fs.scale);

    TyreSaveConfig& ts = g_tyreSaveConfig;
    ts.enabled = ReadBool("TyreSave", "Enabled", ts.enabled);
    ts.targetWearPercent = ReadFloat("TyreSave", "TargetWearPercent", ts.targetWearPercent);
    ts.targetMinutes = ReadFloat("TyreSave", "TargetMinutes", ts.targetMinutes);
    ts.centerXFraction = ReadFloat("TyreSave", "CenterXFraction", ts.centerXFraction);
    ts.centerYFraction = ReadFloat("TyreSave", "CenterYFraction", ts.centerYFraction);
    ts.scale = ReadFloat("TyreSave", "Scale", ts.scale);

    EngineTempsConfig& et = g_engineTempsConfig;
    et.enabled = ReadBool("EngineTemps", "Enabled", et.enabled);
    et.waterAmberMargin = ReadFloat("EngineTemps", "WaterAmberMargin", et.waterAmberMargin);
    et.oilAmberMargin = ReadFloat("EngineTemps", "OilAmberMargin", et.oilAmberMargin);
    et.centerXFraction = ReadFloat("EngineTemps", "CenterXFraction", et.centerXFraction);
    et.centerYFraction = ReadFloat("EngineTemps", "CenterYFraction", et.centerYFraction);
    et.scale = ReadFloat("EngineTemps", "Scale", et.scale);

    FfbLevelConfig& ffl = g_ffbLevelConfig;
    ffl.enabled = ReadBool("FfbLevel", "Enabled", ffl.enabled);
    ffl.centerXFraction = ReadFloat("FfbLevel", "CenterXFraction", ffl.centerXFraction);
    ffl.centerYFraction = ReadFloat("FfbLevel", "CenterYFraction", ffl.centerYFraction);
    ffl.scale = ReadFloat("FfbLevel", "Scale", ffl.scale);

    FuelTyrePanelConfig& ft = g_fuelTyrePanelConfig;
    ft.enabled = ReadBool("FuelTyrePanel", "Enabled", ft.enabled);
    ft.showFuel = ReadBool("FuelTyrePanel", "ShowFuel", ft.showFuel);
    ft.fuelPosXFraction = ReadFloat("FuelTyrePanel", "FuelPosXFraction", ft.fuelPosXFraction);
    ft.fuelPosYFraction = ReadFloat("FuelTyrePanel", "FuelPosYFraction", ft.fuelPosYFraction);
    ft.fuelScale = ReadFloat("FuelTyrePanel", "FuelScale", ft.fuelScale);
    ft.showTyres = ReadBool("FuelTyrePanel", "ShowTyres", ft.showTyres);
    ft.tyresPosXFraction = ReadFloat("FuelTyrePanel", "TyresPosXFraction", ft.tyresPosXFraction);
    ft.tyresPosYFraction = ReadFloat("FuelTyrePanel", "TyresPosYFraction", ft.tyresPosYFraction);
    ft.tyresScale = ReadFloat("FuelTyrePanel", "TyresScale", ft.tyresScale);
    ft.showBrakeBias = ReadBool("FuelTyrePanel", "ShowBrakeBias", ft.showBrakeBias);
    ft.brakeBiasPosXFraction = ReadFloat("FuelTyrePanel", "BrakeBiasPosXFraction", ft.brakeBiasPosXFraction);
    ft.brakeBiasPosYFraction = ReadFloat("FuelTyrePanel", "BrakeBiasPosYFraction", ft.brakeBiasPosYFraction);
    ft.brakeBiasScale = ReadFloat("FuelTyrePanel", "BrakeBiasScale", ft.brakeBiasScale);
    // Dragging or resizing a box in layout mode saves it under its own
    // section (FuelTyrePanel_Fuel etc. -- see FuelTyrePanel.cpp's beginBox),
    // so those win over the keys above; reading only the keys above put the
    // boxes back where they were at every restart.
    ft.fuelPosXFraction = ReadFloat("FuelTyrePanel_Fuel", "PosXFraction", ft.fuelPosXFraction);
    ft.fuelPosYFraction = ReadFloat("FuelTyrePanel_Fuel", "PosYFraction", ft.fuelPosYFraction);
    ft.fuelScale = ReadFloat("FuelTyrePanel_Fuel", "Scale", ft.fuelScale);
    ft.tyresPosXFraction = ReadFloat("FuelTyrePanel_Tyres", "PosXFraction", ft.tyresPosXFraction);
    ft.tyresPosYFraction = ReadFloat("FuelTyrePanel_Tyres", "PosYFraction", ft.tyresPosYFraction);
    ft.tyresScale = ReadFloat("FuelTyrePanel_Tyres", "Scale", ft.tyresScale);
    ft.brakeBiasPosXFraction = ReadFloat("FuelTyrePanel_BrakeBias", "PosXFraction", ft.brakeBiasPosXFraction);
    ft.brakeBiasPosYFraction = ReadFloat("FuelTyrePanel_BrakeBias", "PosYFraction", ft.brakeBiasPosYFraction);
    ft.brakeBiasScale = ReadFloat("FuelTyrePanel_BrakeBias", "Scale", ft.brakeBiasScale);
    ft.textHeightPx = ReadFloat("FuelTyrePanel", "TextHeightPixels", ft.textHeightPx);
    ft.lineGapPx = ReadFloat("FuelTyrePanel", "LineGapPixels", ft.lineGapPx);
    ft.columnGapPx = ReadFloat("FuelTyrePanel", "ColumnGapPixels", ft.columnGapPx);
    ft.colorR = ReadFloat("FuelTyrePanel", "ColorR", ft.colorR);
    ft.colorG = ReadFloat("FuelTyrePanel", "ColorG", ft.colorG);
    ft.colorB = ReadFloat("FuelTyrePanel", "ColorB", ft.colorB);
    ft.alertR = ReadFloat("FuelTyrePanel", "AlertColorR", ft.alertR);
    ft.alertG = ReadFloat("FuelTyrePanel", "AlertColorG", ft.alertG);
    ft.alertB = ReadFloat("FuelTyrePanel", "AlertColorB", ft.alertB);
    ft.tyreLowVsRaceR = ReadFloat("FuelTyrePanel", "TyreLowVsRaceColorR", ft.tyreLowVsRaceR);
    ft.tyreLowVsRaceG = ReadFloat("FuelTyrePanel", "TyreLowVsRaceColorG", ft.tyreLowVsRaceG);
    ft.tyreLowVsRaceB = ReadFloat("FuelTyrePanel", "TyreLowVsRaceColorB", ft.tyreLowVsRaceB);
    ft.tyreLowVsRaceA = ReadFloat("FuelTyrePanel", "TyreLowVsRaceAlpha", ft.tyreLowVsRaceA);
    ft.tyreCriticalVsRaceR = ReadFloat("FuelTyrePanel", "TyreCriticalVsRaceColorR", ft.tyreCriticalVsRaceR);
    ft.tyreCriticalVsRaceG = ReadFloat("FuelTyrePanel", "TyreCriticalVsRaceColorG", ft.tyreCriticalVsRaceG);
    ft.tyreCriticalVsRaceB = ReadFloat("FuelTyrePanel", "TyreCriticalVsRaceColorB", ft.tyreCriticalVsRaceB);
    ft.tyreCriticalVsRaceA = ReadFloat("FuelTyrePanel", "TyreCriticalVsRaceAlpha", ft.tyreCriticalVsRaceA);
    ft.fuelMarginMinutes = ReadFloat("FuelTyrePanel", "FuelMarginMinutes", ft.fuelMarginMinutes);
    ft.wearPlotWidthPx = ReadFloat("FuelTyrePanel", "WearPlotWidthPixels", ft.wearPlotWidthPx);
    ft.wearPlotHeightPx = ReadFloat("FuelTyrePanel", "WearPlotHeightPixels", ft.wearPlotHeightPx);
    ft.wearBigPlotHeightPx = ReadFloat("FuelTyrePanel", "WearBigPlotHeightPixels", ft.wearBigPlotHeightPx);
    ft.fuelMarginLaps = ReadFloat("FuelTyrePanel", "FuelMarginLaps", ft.fuelMarginLaps);
    ft.fuelGoodR = ReadFloat("FuelTyrePanel", "FuelGoodColorR", ft.fuelGoodR);
    ft.fuelGoodG = ReadFloat("FuelTyrePanel", "FuelGoodColorG", ft.fuelGoodG);
    ft.fuelGoodB = ReadFloat("FuelTyrePanel", "FuelGoodColorB", ft.fuelGoodB);
    ft.fuelMarginalR = ReadFloat("FuelTyrePanel", "FuelMarginalColorR", ft.fuelMarginalR);
    ft.fuelMarginalG = ReadFloat("FuelTyrePanel", "FuelMarginalColorG", ft.fuelMarginalG);
    ft.fuelMarginalB = ReadFloat("FuelTyrePanel", "FuelMarginalColorB", ft.fuelMarginalB);
    ft.fuelShortR = ReadFloat("FuelTyrePanel", "FuelShortColorR", ft.fuelShortR);
    ft.fuelShortG = ReadFloat("FuelTyrePanel", "FuelShortColorG", ft.fuelShortG);
    ft.fuelShortB = ReadFloat("FuelTyrePanel", "FuelShortColorB", ft.fuelShortB);
    ft.groupGapPx = ReadFloat("FuelTyrePanel", "GroupGapPixels", ft.groupGapPx);
    ft.tyreFreezeAfterLineSeconds = ReadFloat("FuelTyrePanel", "TyreFreezeAfterLineSeconds", ft.tyreFreezeAfterLineSeconds);
    ft.bgEnabled = ReadBool("FuelTyrePanel", "BackgroundEnabled", ft.bgEnabled);
    ft.bgPaddingPx = ReadFloat("FuelTyrePanel", "BackgroundPaddingPixels", ft.bgPaddingPx);
    ft.bgCornerRadiusPx = ReadFloat("FuelTyrePanel", "BackgroundCornerRadiusPixels", ft.bgCornerRadiusPx);
    ft.bgR = ReadFloat("FuelTyrePanel", "BackgroundR", ft.bgR);
    ft.bgG = ReadFloat("FuelTyrePanel", "BackgroundG", ft.bgG);
    ft.bgB = ReadFloat("FuelTyrePanel", "BackgroundB", ft.bgB);
    ft.bgA = ReadFloat("FuelTyrePanel", "BackgroundA", ft.bgA);

    DrsCueConfig& dc = g_drsCueConfig;
    dc.enabled = ReadBool("DrsCue", "Enabled", dc.enabled);
    dc.beepFrequencyHz = ReadInt("DrsCue", "BeepFrequencyHz", dc.beepFrequencyHz);
    dc.beepDurationMs = ReadInt("DrsCue", "BeepDurationMs", dc.beepDurationMs);
    dc.gapMs = ReadInt("DrsCue", "GapMs", dc.gapMs);

    Log::Write("Config: loaded from %s", s_iniPath);
}

namespace
{
    struct PendingWrite { std::string section, key, value; };
    std::vector<PendingWrite> s_pendingWrites;

    // Upsert by (section, key) -- last write wins, and a key touched several
    // times before the next flush (e.g. nudging a layout position repeatedly)
    // only ever queues one final disk write for it, not one per change.
    void QueueWrite(const char* section, const char* key, const std::string& value)
    {
        for (PendingWrite& w : s_pendingWrites)
        {
            if (w.section == section && w.key == key)
            {
                w.value = value;
                return;
            }
        }
        s_pendingWrites.push_back({ section, key, value });
    }
}

bool Config::WriteBool(const char* section, const char* key, bool value)
{
    if (s_iniPath[0] == 0)
        return false;
    QueueWrite(section, key, value ? "1" : "0");
    return true;
}

void Config::KeyName(int vk, char* out, size_t outSize)
{
    if (vk <= 0)
    {
        sprintf_s(out, outSize, "Unassigned");
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F12)
    {
        sprintf_s(out, outSize, "F%d", vk - VK_F1 + 1);
        return;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        out[0] = static_cast<char>(vk);
        out[1] = 0;
        return;
    }
    sprintf_s(out, outSize, "?");
}

bool Config::WriteFloat(const char* section, const char* key, float value)
{
    if (s_iniPath[0] == 0)
        return false;
    char buf[32];
    sprintf_s(buf, "%.4f", value);
    QueueWrite(section, key, buf);
    return true;
}

bool Config::WriteString(const char* section, const char* key, const char* value)
{
    if (s_iniPath[0] == 0)
        return false;
    QueueWrite(section, key, value);
    return true;
}

float Config::ReadFloatLive(const char* section, const char* key, float def)
{
    return ReadFloat(section, key, def);
}

bool Config::FlushPendingWrites()
{
    if (s_pendingWrites.empty())
        return true;

    // Swapped out before writing (not cleared after) so a write queued by
    // some OTHER thread while this loop is running -- unlikely, since this
    // runs from the same scoring-tick trigger every other FlushIfDirty()
    // does, but not impossible -- lands in the NEXT flush instead of being
    // dropped on the floor.
    std::vector<PendingWrite> batch;
    batch.swap(s_pendingWrites);

    bool allOk = true;
    for (const PendingWrite& w : batch)
    {
        if (!WritePrivateProfileStringA(w.section.c_str(), w.key.c_str(), w.value.c_str(), s_iniPath))
            allOk = false;
    }
    return allOk;
}

namespace
{
    // Standard DirectInput scancodes for the keys anyone would realistically
    // bind a toggle to. Not exhaustive (numpad, media keys and the like are
    // left out) -- an unrecognised code falls back to "not bound" in the
    // caller, same as a genuinely unbound control, since this lookup exists
    // to answer "what do I press", not to catalogue every possible bind.
    const char* DikScanCodeName(int code)
    {
        switch (code)
        {
            case 0x01: return "Esc";
            case 0x02: return "1"; case 0x03: return "2"; case 0x04: return "3";
            case 0x05: return "4"; case 0x06: return "5"; case 0x07: return "6";
            case 0x08: return "7"; case 0x09: return "8"; case 0x0A: return "9";
            case 0x0B: return "0";
            case 0x0F: return "Tab";
            case 0x10: return "Q"; case 0x11: return "W"; case 0x12: return "E";
            case 0x13: return "R"; case 0x14: return "T"; case 0x15: return "Y";
            case 0x16: return "U"; case 0x17: return "I"; case 0x18: return "O";
            case 0x19: return "P";
            case 0x1C: return "Enter";
            case 0x1E: return "A"; case 0x1F: return "S"; case 0x20: return "D";
            case 0x21: return "F"; case 0x22: return "G"; case 0x23: return "H";
            case 0x24: return "J"; case 0x25: return "K"; case 0x26: return "L";
            case 0x2C: return "Z"; case 0x2D: return "X"; case 0x2E: return "C";
            case 0x2F: return "V"; case 0x30: return "B"; case 0x31: return "N";
            case 0x32: return "M";
            case 0x39: return "Space";
            case 0x3B: return "F1"; case 0x3C: return "F2"; case 0x3D: return "F3";
            case 0x3E: return "F4"; case 0x3F: return "F5"; case 0x40: return "F6";
            case 0x41: return "F7"; case 0x42: return "F8"; case 0x43: return "F9";
            case 0x44: return "F10"; case 0x57: return "F11"; case 0x58: return "F12";
            case 0xC7: return "Home"; case 0xC8: return "Up"; case 0xC9: return "PgUp";
            case 0xCB: return "Left"; case 0xCD: return "Right"; case 0xCF: return "End";
            case 0xD0: return "Down"; case 0xD1: return "PgDn"; case 0xD2: return "Insert";
            case 0xD3: return "Delete";
            default: return nullptr;
        }
    }
}

namespace
{
    // Shared by ReadToggleCursorKeyName (wants a display string) and
    // ReadToggleCursorScanCode (wants the raw code to synthesize a keypress).
    // Returns false under every "nothing usable" outcome -- unbound, bound to
    // a non-keyboard device, or Controller.JSON missing/unparseable -- so
    // callers don't need to re-derive that logic themselves.
    bool ReadToggleCursorDikCode(int& outCode)
    {
        // Same "find our own module" trick Load() uses, so this works
        // regardless of where rFactor 2 is installed.
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ReadToggleCursorDikCode), &hSelf);
        char dllPath[MAX_PATH];
        if (GetModuleFileNameA(hSelf, dllPath, MAX_PATH) == 0)
            return false;

        // dllPath is ".../<rFactor 2>/Bin64/Plugins/rF2SkrOverlayPublic.dll" --
        // THREE strrchr passes strip it down to the sim's own root: the
        // filename itself, then "Plugins", then "Bin64" (an earlier version
        // of this only did two passes, landing one level short at
        // ".../Bin64" and never finding the file at all -- silently falling
        // back to "not bound" even when the control genuinely was bound).
        // Then down into the default profile. A driver using a different
        // profile name still just gets "not bound" here, same as if the
        // control genuinely were unbound -- this is a convenience lookup,
        // not a promise to find every profile.
        char path[MAX_PATH];
        strcpy_s(path, dllPath);
        for (int i = 0; i < 3; ++i)
        {
            char* slash = strrchr(path, '\\');
            if (!slash)
                return false;
            *slash = 0;
        }
        strcat_s(path, "\\UserData\\player\\Controller.JSON");

        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f)
            return false;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0 || size > 2 * 1024 * 1024)
        {
            fclose(f);
            return false;
        }
        char* buf = new char[static_cast<size_t>(size) + 1];
        const size_t readBytes = fread(buf, 1, static_cast<size_t>(size), f);
        buf[readBytes] = 0;
        fclose(f);

        bool found = false;
        const char* key = strstr(buf, "\"Control - Toggle Cursor\"");
        if (key)
        {
            const char* bracket = strchr(key, '[');
            int device = -1, code = -1;
            if (bracket && sscanf_s(bracket, "[%d,%d", &device, &code) == 2)
            {
                // rF2's own sentinel for "nothing bound here", verified
                // against this project's own Controller.JSON.
                if (device == 0 && code != 89)
                {
                    outCode = code;
                    found = true;
                }
            }
        }
        delete[] buf;
        return found;
    }
}

void Config::ReadToggleCursorKeyName(char* out, size_t outSize)
{
    sprintf_s(out, outSize, "not bound");

    int code = -1;
    if (ReadToggleCursorDikCode(code))
    {
        const char* name = DikScanCodeName(code);
        if (name)
            sprintf_s(out, outSize, "%s", name);
    }
}

bool Config::ReadToggleCursorScanCode(int& outCode)
{
    return ReadToggleCursorDikCode(outCode);
}
