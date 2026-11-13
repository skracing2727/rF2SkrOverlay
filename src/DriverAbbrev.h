#pragma once
#include <Windows.h>
#include <cstddef>

// Declared at GLOBAL scope on purpose: writing "const struct ScoringInfoV01&"
// inside the namespace below would declare a DIFFERENT, incomplete type
// DriverAbbrev::ScoringInfoV01 rather than referring to the SDK one.
struct ScoringInfoV01;

// Three-letter driver codes (VER, HAM, LEC) for labelling car dots on the
// track map, and for any table too narrow to carry a full name.
//
// The abbreviation itself is the easy half. Two things make this worth its
// own module:
//
// UNIQUENESS. Two drivers must never share a code, or the map lies about who
// is where. Codes are therefore assigned against the WHOLE field at once,
// with collisions resolved by falling back through a fixed ladder of
// alternatives (Michael and Ralf Schumacher become MSC and RSC rather than
// both being SCH).
//
// STABILITY. rFactor 2 REORDERS ScoringInfoV01::mVehicle[] between ticks --
// generally by running order -- as CarState.h already documents. Resolving
// collisions in array order would therefore hand two colliding drivers each
// other's codes the moment they swap positions, so a label would silently
// change mid-race. Assignment instead walks the field sorted by mID, which is
// stable per car for the life of the session, so a code depends only on WHO
// is in the session and not on where they currently are.
//
// The renderer's font atlas is ASCII 32..126 only, and Renderer::AddText
// silently SKIPS anything outside it rather than drawing a placeholder. So
// "Raikkonen" spelled with the real glyphs would abbreviate to RIK -- wrong,
// and wrong invisibly. Names are therefore folded to ASCII first, accepting
// either UTF-8 or Windows-1252 input since which one rFactor 2 hands over
// has not been confirmed.
namespace DriverAbbrev
{
    // Called from the scoring thread every tick. Cheap when nothing has
    // changed: the whole field is only re-abbreviated when the set of cars or
    // one of their names actually differs from last time.
    void OnScoring(const ScoringInfoV01& info);

    // Copies this car's code into 'out' (needs >= 4 bytes). Returns false and
    // leaves 'out' empty for a car that has not been seen in scoring yet.
    bool Get(long id, char* out, size_t outSize);

    // A stable, visually distinct colour per car, so a dot on the map can be
    // told from its neighbours at a glance.
    //
    // Assigned from the same mID-ordered pass as the codes, for the same
    // reason: array order changes with running order, so assigning from it
    // would have cars swapping colours as they swap places.
    //
    // Assignments are also STICKY across field changes -- a car keeps the
    // colour it was first given even when someone joins or leaves, which is
    // what "assigned at the start of the session" actually has to mean once
    // the field can change. Only genuinely new cars draw a new colour.
    //
    // Returns false for a car not seen in scoring yet, leaving the outputs
    // untouched.
    bool Color(long id, float& r, float& g, float& b);

    // 1-based finishing order for a car, or 0 if unknown. Lives here because
    // it comes from the same scoring pass and the map needs it to decide
    // which of two overlapping labels to keep.
    int Place(long id);

    // The driver's name split for a two-line display: given name(s) on one
    // line, surname on the other.
    //
    // ASCII-FOLDED, like the codes -- the font atlas is ASCII 32..126 only
    // and silently skips anything outside it, so an unfolded accented name
    // would lose characters rather than show a placeholder. Uppercase for
    // the same reason the codes are: it is what the folding produces, and it
    // matches the rest of the panel.
    //
    // 'first' gets everything before the surname (empty for a one-word
    // name); 'last' gets the surname, chosen the same way the code's base is
    // -- the final token, skipping a generational suffix like JR or III.
    // Returns false when the ID is unknown.
    bool NameParts(long id, char* first, size_t firstSize, char* last, size_t lastSize);

    void Clear();
}
