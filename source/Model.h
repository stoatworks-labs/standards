#pragma once

#include <cstdint>

/**
	The conversion as arithmetic. No GL, no pixels.

	A field-store standards converter does two interpolations and knows
	nothing about motion. Everything here is the bookkeeping of those two:
	WHEN each field is (exact rationals, never a float clock), WHICH source
	fields and weights make each destination field, and WHERE each destination
	line falls among a source field's lines. The GPU only multiplies and adds
	what this file decides.

	-------------------------------------------------------------- time

	Field k of a standard with field rate R = num / den is at k den / num
	seconds after the plugin's first frame. A destination field j therefore
	sits at a SOURCE position

	    p_j = j * ( Rd_den Rs_num ) / ( Rd_num Rs_den )

	in source fields, which is a rational with a small denominator. For
	50 -> 60 it is 5j/6, so every weight repeats exactly every six destination
	fields; for 50 -> 59.94 it is 1001j/1200, so after six fields the phase
	has moved on by 6 x 1001/1200 - 5 = 0.005 of a source field: the creep.
	Both are computed in integers, so "repeats exactly" is exact.

	-------------------------------------------------------------- space

	Both standards' active pictures cover the same height. Frame line m of a
	standard with L active lines has its centre at ( m + 1/2 ) / L of the
	height, top first; field parity q holds the frame lines m = 2i + q (top
	field first, in both standards). So destination frame line m falls, in a
	source field of parity p, at field-line coordinate

	    u = ( ( 2m + 1 ) Ls - Ld - 2 p Ld ) / ( 4 Ld )

	Again a rational. The taps read the lines that exist in the field being
	read -- that is what "respect field parity" means, and dropping the
	`- 2 p Ld` term is the classic bug `kPerturbIgnoreParity` reproduces.
*/
namespace standards::model
{

/// Fields per second as an exact ratio.
struct Rate
{
	int64_t num;
	int64_t den;
};

struct Standard
{
	const char* name;
	int lines;     ///< active frame lines
	int fieldLines;///< lines per field, lines / 2
	Rate fieldRate;
};

/// 625 lines, 576 active, 50 fields.
extern const Standard k625;
/// 525 lines, 480 active, 60000/1001 fields.
extern const Standard k525;
/// 525 lines, 480 active, exactly 60 fields (the monochrome rate).
extern const Standard k525At60;

/// What Direction stores. A list of four, in the order the spec names them.
enum Direction
{
	kDir625To525    = 0,///< 625/50 -> 525/59.94
	kDir525To625    = 1,///< 525/59.94 -> 625/50
	kDir625To525At60 = 2,///< 625/50 -> 525/60
	kDirSame        = 3,///< 625/50 -> 625/50: the field store, and nothing else
	kDirectionCount
};

/// What Temporal stores.
enum Temporal
{
	kDropRepeat = 0,///< the nearest source field, ties to the earlier
	kLinear     = 1,///< the two fields either side, weighted by distance
	kFourField  = 2,///< four fields, Catmull-Rom (Keys, a = -1/2) in time
	kTemporalCount
};

/// Vertical Taps, by option index: 1, 2, 4 or 8.
constexpr int kTapCounts[] = { 1, 2, 4, 8 };
constexpr int kTapOptionCount = 4;

/// The longest composite vertical filter: 8 taps with Softness's 3-tap
/// pre-filter convolved in front of it.
constexpr int kMaxLineTaps = 10;

/**
	Negative-control hooks. Each bit breaks the model in one stated way so the
	harness can show a check FAILS against it. The shipped plugin carries 0.
*/
enum Perturb : int
{
	kPerturbSwapWeights   = 1 << 0,///< temporal weights reversed (the spec's)
	kPerturbIgnoreParity  = 1 << 1,///< line positions ignore the source field's parity (the spec's)
	kPerturbFloorRepeat   = 1 << 2,///< Drop/Repeat takes the field at or before, not the nearest
	kPerturbNoCreep       = 1 << 3,///< 59.94 treated as exactly 60 in the weights
	kPerturbKernelDetune  = 1 << 4,///< Catmull-Rom with a = -3/4 instead of -1/2
	kPerturbCaptureBefore = 1 << 5,///< a source field takes the host frame at or before its instant
	kPerturbMcZero        = 1 << 6,///< motion vectors estimated and never applied
	kPerturbMcSign        = 1 << 7,///< motion vectors applied with the wrong sign
	kPerturbResizeClears  = 1 << 8,///< a resize re-primes the store from the current frame
	kPerturbClockFloat    = 1 << 9,///< the field clock kept in float, as the host's clock is not
};

struct Conversion
{
	Standard source;
	Standard destination;
};

Conversion ConversionOf( int direction, int perturb = 0 );

/// floor( a / b ) and its remainder for b > 0, whatever the sign of a. C++
/// and GLSL both give a negative left operand a negative remainder, and the
/// field index is negative while the store is being primed.
int64_t FloorDiv( int64_t a, int64_t b );
int64_t FloorMod( int64_t a, int64_t b );

/// 0 for a top field, 1 for a bottom field. Top field first, in both
/// standards.
int FieldParity( int64_t field );

/// Seconds after the plugin's first frame at which field `k` is.
double FieldTime( int64_t k, Rate rate );

/// A position n + rem / den, 0 <= rem < den, held exactly.
struct Position
{
	int64_t whole;
	int64_t rem;
	int64_t den;

	double Fraction() const
	{
		return static_cast< double >( rem ) / static_cast< double >( den );
	}
	double Value() const
	{
		return static_cast< double >( whole ) + Fraction();
	}
};

/// p_j, destination field j's position in source fields.
Position SourcePositionOf( int64_t j, Rate source, Rate destination );

/// Which source fields make destination field j, and how much of each.
struct TemporalTaps
{
	int count;       ///< 1, 2 or 4
	int64_t first;   ///< the earliest source field used
	double weight[ 4 ];
	double position; ///< p_j, for motion compensation's offsets
};

TemporalTaps TemporalWeights( int64_t j, const Conversion& c, int temporal, int perturb = 0 );

/// Destination field j is produced once this source field exists: floor( p_j )
/// + 2, whatever Temporal is, so the latency does not jump when an operator
/// changes the aperture.
int64_t RequiredSource( int64_t j, const Conversion& c, int perturb = 0 );

/// The first destination field whose whole aperture lies at or after source
/// field `firstSource`.
int64_t FirstDestinationFrom( int64_t firstSource, const Conversion& c, int perturb = 0 );

/// u for destination frame line m in a source field of parity p, exactly.
Position SourceLineOf( int m, int p, const Conversion& c, int perturb = 0 );

/// The continuous interpolation kernel for `taps` taps, evaluated at
/// distance x: a box for 1, a triangle for 2, Catmull-Rom for 4, Lanczos-4
/// for 8. Used for time (4) and for lines.
double Kernel( int taps, double x, int perturb = 0 );

/// The vertical filter for destination frame line m from a source field of
/// parity p: `count` weights on field lines first .. first + count - 1.
/// Softness convolves a [ a, 1 - 2a, a ] pre-filter in front, a = Softness / 4.
struct LineTaps
{
	int first;
	int count;
	double weight[ kMaxLineTaps ];
};

LineTaps VerticalTaps( int m, int p, const Conversion& c, int taps, double softness, int perturb = 0 );

} // namespace standards::model
