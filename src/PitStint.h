#pragma once

struct ScoringInfoV01;

// Laps since each car's last pit stop, for the standings' "L12" column.
//
// Each scoring tick notes, per car, when it leaves the pit lane after a real
// stop and its completed-lap count at that moment; laps since the stop = laps
// now - that. A drive-through, or a visit under 2 s (mInPits flicker on
// remote cars), does not count as a stop.
// A car with no pit stops yet (mNumPitstops == 0) counts from the start. A car
// that has stopped before the plugin first saw it is unknown ("?") until its
// next stop.
//
// Cars are keyed by slot ID plus vehicle (entry) name, not driver, so a
// driver swap in an endurance race keeps the count, and two cars with the
// same default livery name no longer share one record. Kept in memory for the session
// only -- nothing is written to disk (it used to be saved to a file on every
// pit exit; laps since a stop mean nothing outside the session they were
// counted in).
namespace PitStint
{
    void OnScoring(const ScoringInfoV01& info);   // scoring thread
    // Laps since this car's last stop; false = not known.
    bool Get(long carId, int& laps);
}
