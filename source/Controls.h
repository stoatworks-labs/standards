#pragma once

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range.
*/
namespace standards::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Vertical Taps: 1, 2, 4 or 8.
int TapsFromOption( float value );

/// Softness: the pre-filter's side weight is a quarter of it, so 1 is
/// [ 1/4, 1/2, 1/4 ] and 0 is no pre-filter at all.
double SoftnessFromParam( float value );

/// What Show As stores.
enum ShowAs
{
	kShowWeave = 0,
	kShowBob   = 1,
	kShowCount
};

/// What Output Size stores.
enum OutputSize
{
	kOutputNative = 0,///< one output row per destination line, centred
	kOutputHost   = 1,///< the destination frame scaled to the host's height
	kOutputCount
};

} // namespace standards::controls
