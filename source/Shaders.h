#pragma once

/**
	The six passes. Every one of them reads with `texelFetch` at integer
	coordinates computed in integers: a field is a set of rows, two adjacent
	rows of a frame are two different moments, and a bilinear sample across
	them is a sample across time (cadence's first trap). Every interpolation
	this plugin does, it does itself, with weights the CPU computed in double
	from exact rationals (`Model.h`) -- so nothing here depends on a texture
	unit's filtering precision or on where a rasteriser's interpolated uv
	happens to land.

	Rows in every buffer this plugin owns are LINES, top first: texel row i is
	line i. Only the capture (reading the host's picture) and the display
	(writing it) know that GL's row 0 is the bottom.

	  capture   host picture   -> the source standard's frame lines (W x Ls)
	  field     frame lines    -> one field's lines, by parity (W x Ls/2)
	  resample  a buffer       -> the same buffer at a new width (a resize)
	  motion    two fields     -> block vectors (crude, SAD, integer)
	  convert   1-4 fields     -> one destination field (W x Ld/2)
	  display   a field pair   -> the host's framebuffer, weave or bob
*/
namespace standards::shaders
{

extern const char* const kVertex;
extern const char* const kCapture;
extern const char* const kField;
extern const char* const kResample;
extern const char* const kMotion;
extern const char* const kConvert;
extern const char* const kDisplay;

/// Block size and search range of the motion pass, shared with the C++.
constexpr int kBlockWidth  = 16;
constexpr int kBlockLines  = 8;
constexpr int kSearchX     = 16;
constexpr int kSearchLines = 2;

} // namespace standards::shaders
