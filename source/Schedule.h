#pragma once

#include "Model.h"

#include <cstdint>
#include <vector>

/**
	Which fields happen on this host frame. No GL: the plugin asks, and does
	what it is told; the offline harness asks the same questions with a
	six-day host clock and checks the answers do not change.

	**Source fields** are at their own standard's exact instants, n / Rs. A
	host frame arriving at `now` makes every field whose instant has passed
	since the last one, each from the NEAREST of the two host frames either
	side of its instant, as a camera with a shutter would (a tie goes to the
	earlier). That needs the previous frame, which the plugin keeps.

	**Destination fields** are at THEIR standard's instants, j / Rd, released
	a fixed two source fields late: the four-field aperture reaches two source
	fields past p_j, so that is the latency a real converter has too. Released
	by time, not by "the source it needs has arrived" -- releasing by arrival
	would hand the display fields in bursts of two and add a judder of its own
	that is not the converter's.

	**Priming.** The first frame (and the first after a reset) fills the store
	with that picture as seven fields, as if it had been still forever, so the
	converter has an output on the frame it starts rather than after two
	fields of nothing -- and nothing reads as having "risen from black".
*/
namespace standards
{
class Schedule
{
public:
	struct Capture
	{
		int64_t field;
		bool fromCurrent;///< the frame that just arrived, rather than the one before
	};

	/// Fields the store is primed with, ending at the field due now.
	static constexpr int kPrimeFields = 7;
	/// More than this many due in one frame is a stall: the oldest are skipped.
	static constexpr int kMaxPerFrame = 8;
	/// Equality, for instants the harness places exactly and double rounds.
	static constexpr double kEpsilon = 1e-7;

	void Reset()
	{
		primed = false;
	}

	bool Primed() const
	{
		return primed;
	}

	/// A host frame at `now`. On the first, the priming captures; after that,
	/// the fields due since the last frame. `primedNow` says which happened.
	void Frame( double now, const model::Conversion& c, int perturb, std::vector< Capture >& captures, bool& primedNow );

	/// Destination fields due by `now`, oldest first.
	void Destinations( double now, const model::Conversion& c, int perturb, std::vector< int64_t >& out );

	int64_t NewestSource() const
	{
		return newestSource;
	}
	int64_t LatestDestination() const
	{
		return latestDest;
	}
	bool HasDestination() const
	{
		return latestDest != kNone;
	}

	/// The destination latency: two source fields.
	static double Delay( const model::Conversion& c );

private:
	static constexpr int64_t kNone = INT64_MIN;

	bool primed          = false;
	double prevTime      = 0.0;
	int64_t nextSource   = 0;
	int64_t newestSource = 0;
	int64_t nextDest     = 0;
	int64_t latestDest   = kNone;
};

} // namespace standards
