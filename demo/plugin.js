/**
 * Standards — browser demo.
 *
 * A field-store standards converter, PAL to NTSC and back. The one idea, from
 * `source/Standards.h`: until the 1990s a programme crossing the Atlantic went
 * through a box that turned 625-line, 50-field pictures into 525-line,
 * 59.94-field ones with a store of a few fields and two interpolations — in
 * time and in space — neither of which knows anything about motion. The look
 * falls out of that: a judder cycle six fields long (50 into 60 is 5 into 6),
 * creeping at 59.94; double images where an output field lands between two
 * input fields; the soft look of few vertical taps across interlaced lines.
 *
 * The plugin is two processors, and so is the page — not equally faithfully:
 *
 *   The shaders are the plugin's. `VERTEX`, `CAPTURE`, `FIELD`, `RESAMPLE`,
 *   `MOTION_SAD`, `MOTION_PICK`, `CONVERT` and `DISPLAY` below are `kVertex` …
 *   `kDisplay` from `source/Shaders.cpp`, copied across unedited.
 *   `demo/tools/check_shaders.py` compares them character for character and
 *   `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT — of `Model.cpp` (exact field positions as
 *   rationals, the temporal weights, the vertical filter table),
 *   `Schedule.cpp` (which source and destination fields happen on this host
 *   frame), `Controls.cpp`, `Clock.cpp` and the store bookkeeping in
 *   `Standards::ProcessOpenGL` — function for function. Nothing checks a port
 *   but a reader. `sttest --model`, `--weights`, `--judder` and the rest check
 *   the C++ originals and have no idea this page exists.
 *
 * There is no readback: the CPU decides numbers and the GPU does every pixel,
 * so the only traffic from CPU to GPU is uniforms and the filter table, as in
 * the plugin.
 *
 * ------------------------------------------------------- the clock
 *
 * Fields are instants, and the page's instants come from the kit's `time` —
 * seconds since the page started, paused by Pause, stepped by Step — fed to
 * the ported `Clock` with the unit DECLARED as seconds, as `sttest` declares
 * its own. The plugin's vote on Resolume's clock unit never runs here. A
 * browser paints at the display's rate, commonly 60 Hz and not locked to
 * either standard, so how the source fields fall between host frames is
 * whatever this machine's display makes it; the plugin is in the same
 * position inside Resolume.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Nothing audio.** Standards has no audio path.
 * **The About block is absent**, as on every page in this suite.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const CAPTURE = `#version 410 core

uniform sampler2D InputTexture;
uniform int InWidth;   //the picture, in texels of the input texture
uniform int InHeight;
uniform int Width;     //the store's width
uniform int Lines;     //the source standard's active lines

out vec4 fragColor;

void main()
{
	int x = int( gl_FragCoord.x );
	int l = int( gl_FragCoord.y );

	int r  = ( ( 2 * l + 1 ) * InHeight ) / ( 2 * Lines );
	int xh = ( ( 2 * x + 1 ) * InWidth ) / ( 2 * Width );

	fragColor = texelFetch( InputTexture, ivec2( xh, InHeight - 1 - r ), 0 );
}
`;

const FIELD = `#version 410 core

uniform sampler2D FrameLines;
uniform int Parity;

out vec4 fragColor;

void main()
{
	int x = int( gl_FragCoord.x );
	int k = int( gl_FragCoord.y );
	fragColor = texelFetch( FrameLines, ivec2( x, 2 * k + Parity ), 0 );
}
`;

const RESAMPLE = `#version 410 core

uniform sampler2D Source;
uniform int OldWidth;
uniform int NewWidth;

out vec4 fragColor;

void main()
{
	int x  = int( gl_FragCoord.x );
	int y  = int( gl_FragCoord.y );
	int xo = ( ( 2 * x + 1 ) * OldWidth ) / ( 2 * NewWidth );
	fragColor = texelFetch( Source, ivec2( xo, y ), 0 );
}
`;

const MOTION_SAD = `#version 410 core

uniform sampler2D FieldA;
uniform sampler2D FieldB;
uniform int ParityA;
uniform int ParityB;
uniform int Width;
uniform int FieldLines;
uniform int BlockWidth;
uniform int BlockLines;
uniform int SearchX;
uniform int SearchLines;
uniform float Lambda;   //cost per unit of |v|: breaks ties on flat blocks towards zero

out vec4 fragColor;

float luma( vec4 c )
{
	return dot( c.rgb, vec3( 0.299, 0.587, 0.114 ) );
}

float lumaAt( sampler2D field, int x, int k )
{
	return luma( texelFetch( field, ivec2( clamp( x, 0, Width - 1 ), clamp( k, 0, FieldLines - 1 ) ), 0 ) );
}

//B's picture at A's line k.
float bAtLineOfA( int x, int k )
{
	if( ParityA == ParityB )
		return lumaAt( FieldB, x, k );
	if( ParityA == 0 )
		return 0.5 * ( lumaAt( FieldB, x, k - 1 ) + lumaAt( FieldB, x, k ) );
	return 0.5 * ( lumaAt( FieldB, x, k ) + lumaAt( FieldB, x, k + 1 ) );
}

void main()
{
	int across = 2 * SearchX + 1;
	int down   = 2 * SearchLines + 1;
	int gx     = int( gl_FragCoord.x );
	int gy     = int( gl_FragCoord.y );
	int dx     = gx - ( gx / across ) * across - SearchX;
	int dy     = gy - ( gy / down ) * down - SearchLines;
	int x0     = ( gx / across ) * BlockWidth;
	int k0     = ( gy / down ) * BlockLines;

	float sad = 0.0;
	for( int yy = 0; yy < BlockLines; ++yy )
		for( int xx = 0; xx < BlockWidth; ++xx )
			sad += abs( lumaAt( FieldA, x0 + xx, k0 + yy ) - bAtLineOfA( x0 + xx + dx, k0 + yy + dy ) );

	fragColor = vec4( sad + Lambda * float( abs( dx ) + abs( dy ) ), 0.0, 0.0, 1.0 );
}
`;

const MOTION_PICK = `#version 410 core

uniform sampler2D Costs;
uniform int SearchX;
uniform int SearchLines;

out vec4 fragColor;

void main()
{
	int across = 2 * SearchX + 1;
	int down   = 2 * SearchLines + 1;
	int bx     = int( gl_FragCoord.x );
	int by     = int( gl_FragCoord.y );

	//Scanned dy then dx, least first, keeping the first of equals: the same
	//order a single loop would have, so a tie always resolves the same way.
	float best  = 1e30;
	vec2 vector = vec2( 0.0 );
	for( int j = 0; j < down; ++j )
		for( int i = 0; i < across; ++i )
		{
			float cost = texelFetch( Costs, ivec2( bx * across + i, by * down + j ), 0 ).r;
			if( cost < best )
			{
				best   = cost;
				vector = vec2( float( i - SearchX ), float( j - SearchLines ) );
			}
		}

	fragColor = vec4( vector, best, 1.0 );
}
`;

const CONVERT = `#version 410 core

uniform sampler2D Field0;
uniform sampler2D Field1;
uniform sampler2D Field2;
uniform sampler2D Field3;
uniform int Row0;
uniform int Row1;
uniform int Row2;
uniform int Row3;
uniform int Count;
uniform vec4 Weights;

uniform sampler2D Table;
uniform int Width;
uniform int SourceLines;  //lines per source field
uniform int DestLines;    //lines per destination field

uniform int UseMC;
uniform sampler2D Vectors;
uniform int BlocksX;
uniform int BlocksY;
uniform int BlockWidth;
uniform int BlockLines;
uniform vec4 Offsets;     //source field k's distance from the destination instant, in fields
uniform float VectorSign; //1, or -1 for the negative control

out vec4 fragColor;

vec4 fetchField( int k, int x, int line )
{
	ivec2 at = ivec2( clamp( x, 0, Width - 1 ), clamp( line, 0, SourceLines - 1 ) );
	if( k == 0 )
		return texelFetch( Field0, at, 0 );
	if( k == 1 )
		return texelFetch( Field1, at, 0 );
	if( k == 2 )
		return texelFetch( Field2, at, 0 );
	return texelFetch( Field3, at, 0 );
}

vec4 column( int k, int x, int first, int count, float w[ 10 ] )
{
	vec4 sum = vec4( 0.0 );
	for( int t = 0; t < count; ++t )
		sum += w[ t ] * fetchField( k, x, first + t );
	return sum;
}

vec4 filtered( int k, int row, int x, vec2 v, float offset )
{
	vec4 a = texelFetch( Table, ivec2( 0, row ), 0 );
	vec4 b = texelFetch( Table, ivec2( 1, row ), 0 );
	vec4 c = texelFetch( Table, ivec2( 2, row ), 0 );
	float w[ 10 ] = float[ 10 ]( a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w );
	int first     = int( a.x );
	int count     = int( a.y );

	if( UseMC == 0 )
		return column( k, x, first, count, w );

	//Where this field saw what is at x now: the vector times the field's
	//distance from the destination instant.
	float xs = float( x ) + VectorSign * offset * v.x;
	int dy   = int( floor( VectorSign * offset * v.y + 0.5 ) );
	int xa   = int( floor( xs ) );
	float fx = xs - float( xa );

	vec4 left = column( k, xa, first + dy, count, w );
	if( fx <= 0.0 )
		return left;
	return left * ( 1.0 - fx ) + fx * column( k, xa + 1, first + dy, count, w );
}

void main()
{
	int x = int( gl_FragCoord.x );
	int i = int( gl_FragCoord.y );

	vec2 v = vec2( 0.0 );
	if( UseMC != 0 )
	{
		int bx = clamp( x / BlockWidth, 0, BlocksX - 1 );
		int by = clamp( ( ( i * SourceLines ) / DestLines ) / BlockLines, 0, BlocksY - 1 );
		v      = texelFetch( Vectors, ivec2( bx, by ), 0 ).xy;
	}

	vec4 sum = vec4( 0.0 );
	if( Count > 0 )
		sum += Weights.x * filtered( 0, Row0 + i, x, v, Offsets.x );
	if( Count > 1 )
		sum += Weights.y * filtered( 1, Row1 + i, x, v, Offsets.y );
	if( Count > 2 )
		sum += Weights.z * filtered( 2, Row2 + i, x, v, Offsets.z );
	if( Count > 3 )
		sum += Weights.w * filtered( 3, Row3 + i, x, v, Offsets.w );

	fragColor = sum;
}
`;

const DISPLAY = `#version 410 core

uniform sampler2D FieldA;  //older field of the pair
uniform sampler2D FieldB;  //newer field; the one Bob shows
uniform int ParityA;
uniform int ParityB;

uniform int ShowAs;        //0 weave, 1 bob
uniform int OutputSize;    //0 native lines, 1 host
uniform int NativeOffset;  //floor( ( H - Ld ) / 2 ), from the CPU
uniform int VpX;
uniform int VpY;
uniform int VpW;
uniform int VpH;
uniform int Width;         //the store's width
uniform int FrameLines;    //the destination standard's active lines
uniform int FieldLines;

uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

vec4 lineOf( sampler2D field, int x, int k )
{
	return texelFetch( field, ivec2( x, clamp( k, 0, FieldLines - 1 ) ), 0 );
}

void main()
{
	int X  = int( gl_FragCoord.x ) - VpX;
	int Yb = int( gl_FragCoord.y ) - VpY;
	int Y  = VpH - 1 - Yb;

	int m = OutputSize == 1 ? ( ( 2 * Y + 1 ) * FrameLines ) / ( 2 * VpH ) : Y - NativeOffset;
	int x = clamp( ( ( 2 * X + 1 ) * Width ) / ( 2 * VpW ), 0, Width - 1 );

	vec4 converted;
	if( m < 0 || m >= FrameLines )
		converted = vec4( 0.0, 0.0, 0.0, 1.0 );
	else if( ShowAs == 0 )
		converted = ( m & 1 ) == ParityB ? lineOf( FieldB, x, m >> 1 ) : lineOf( FieldA, x, m >> 1 );
	else if( ( m & 1 ) == ParityB )
		converted = lineOf( FieldB, x, m >> 1 );
	else
	{
		//The field's own lines either side of m: frame lines m - 1 and m + 1,
		//which are field lines ( m - 1 ) >> 1 and ( m + 1 ) >> 1. At the top
		//and bottom only one exists.
		bool above = m - 1 >= 0;
		bool below = m + 1 < FrameLines;
		if( above && below )
			converted = 0.5 * ( lineOf( FieldB, x, ( m - 1 ) >> 1 ) + lineOf( FieldB, x, ( m + 1 ) >> 1 ) );
		else if( above )
			converted = lineOf( FieldB, x, ( m - 1 ) >> 1 );
		else
			converted = lineOf( FieldB, x, ( m + 1 ) >> 1 );
	}

	if( MixAmount >= 1.0 )
	{
		fragColor = converted;
		return;
	}
	vec4 source = texture( InputTexture, uv * MaxUV );
	fragColor   = mix( source, converted, MixAmount );
}
`;

/// Block size and search range of the motion pass, from Shaders.h.
const K_BLOCK_WIDTH = 16;
const K_BLOCK_LINES = 8;
const K_SEARCH_X = 16;
const K_SEARCH_LINES = 2;

//===========================================================================
// Model.cpp, ported. Every position is an exact rational; the integers here
// stay far inside 2^53, where a JavaScript number is an exact integer.
//===========================================================================

const K625 = { name: '625/50', lines: 576, fieldLines: 288, fieldRate: { num: 50, den: 1 } };
const K525 = { name: '525/59.94', lines: 480, fieldLines: 240, fieldRate: { num: 60000, den: 1001 } };
const K525_AT_60 = { name: '525/60', lines: 480, fieldLines: 240, fieldRate: { num: 60, den: 1 } };

const DIR_625_TO_525 = 0;
const DIR_525_TO_625 = 1;
const DIR_625_TO_525_AT_60 = 2;
const DIR_SAME = 3;
const DIRECTION_COUNT = 4;

const DROP_REPEAT = 0;
const LINEAR = 1;
const FOUR_FIELD = 2;
const TEMPORAL_COUNT = 3;

const TAP_COUNTS = [1, 2, 4, 8];
const TAP_OPTION_COUNT = 4;
const MAX_LINE_TAPS = 10;

function conversionOf(direction) {
  switch (direction) {
    case DIR_525_TO_625: return { source: K525, destination: K625 };
    case DIR_625_TO_525_AT_60: return { source: K625, destination: K525_AT_60 };
    case DIR_SAME: return { source: K625, destination: K625 };
    case DIR_625_TO_525:
    default: return { source: K625, destination: K525 };
  }
}

function floorDiv(a, b) {
  let q = Math.trunc(a / b);
  if (a % b !== 0 && a < 0) q -= 1;
  return q;
}

function floorMod(a, b) {
  const r = a % b;
  return r < 0 ? r + b : r;
}

/// 0 for a top field, 1 for a bottom field. Top field first in both standards.
const fieldParity = (field) => floorMod(field, 2);

const fieldTime = (k, rate) => (k * rate.den) / rate.num;

/// Keys' cubic convolution kernel; a = -1/2 is Catmull-Rom.
function keys(x, a) {
  x = Math.abs(x);
  if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
  if (x < 2.0) return ((a * x - 5.0 * a) * x + 8.0 * a) * x - 4.0 * a;
  return 0.0;
}

function sinc(x) {
  if (x === 0.0) return 1.0;
  const px = Math.PI * x;
  return Math.sin(px) / px;
}

function reduce(r) {
  let a = r.num;
  let b = r.den;
  while (b !== 0) {
    const t = a % b;
    a = b;
    b = t;
  }
  return { num: r.num / a, den: r.den / a };
}

const positionFraction = (p) => p.rem / p.den;
const positionValue = (p) => p.whole + positionFraction(p);

/// p_j, destination field j's position in source fields.
function sourcePositionOf(j, source, destination) {
  const ratio = reduce({ num: destination.den * source.num, den: destination.num * source.den });
  const numerator = j * ratio.num;
  const whole = floorDiv(numerator, ratio.den);
  return { whole, rem: numerator - whole * ratio.den, den: ratio.den };
}

function temporalWeights(j, c, temporal) {
  const p = sourcePositionOf(j, c.source.fieldRate, c.destination.fieldRate);
  const phi = positionFraction(p);
  const t = { count: 0, first: 0, weight: [0, 0, 0, 0], position: positionValue(p) };

  switch (temporal) {
    case DROP_REPEAT: {
      // The nearest source field; exactly half-way repeats the earlier one.
      // Compared in integers: 2 rem > den, not phi > 0.5.
      const later = 2 * p.rem > p.den;
      t.count = 1;
      t.first = p.whole + (later ? 1 : 0);
      t.weight[0] = 1.0;
      break;
    }
    case FOUR_FIELD: {
      const a = -0.5;
      t.count = 4;
      t.first = p.whole - 1;
      t.weight[0] = keys(1.0 + phi, a);
      t.weight[1] = keys(phi, a);
      t.weight[2] = keys(1.0 - phi, a);
      t.weight[3] = keys(2.0 - phi, a);
      break;
    }
    case LINEAR:
    default:
      t.count = 2;
      t.first = p.whole;
      t.weight[0] = 1.0 - phi;
      t.weight[1] = phi;
      break;
  }
  return t;
}

/// Destination field j is produced once source field floor( p_j ) + 2 exists.
const requiredSource = (j, c) => sourcePositionOf(j, c.source.fieldRate, c.destination.fieldRate).whole + 2;

/// The first destination field whose whole aperture lies at or after firstSource.
function firstDestinationFrom(firstSource, c) {
  const s = c.source.fieldRate;
  const d = c.destination.fieldRate;
  const ratio = reduce({ num: d.den * s.num, den: d.num * s.den });
  const target = (firstSource + 1) * ratio.den;
  return -floorDiv(-target, ratio.num);
}

/// u for destination frame line m in a source field of parity p, exactly.
function sourceLineOf(m, p, c) {
  const ls = c.source.lines;
  const ld = c.destination.lines;
  const numerator = (2 * m + 1) * ls - ld - 2 * p * ld;
  const den = 4 * ld;
  const whole = floorDiv(numerator, den);
  return { whole, rem: numerator - whole * den, den };
}

function kernel(taps, x) {
  switch (taps) {
    case 1: return x >= -0.5 && x < 0.5 ? 1.0 : 0.0;
    case 2: return Math.abs(x) < 1.0 ? 1.0 - Math.abs(x) : 0.0;
    case 4: return keys(x, -0.5);
    case 8:
    default: return Math.abs(x) < 4.0 ? sinc(x) * sinc(x / 4.0) : 0.0;
  }
}

function verticalTaps(m, p, c, taps, softness) {
  const u = sourceLineOf(m, p, c);
  const phi = positionFraction(u);

  const base = { first: 0, count: 0, weight: new Array(MAX_LINE_TAPS).fill(0) };
  switch (taps) {
    case 1:
      base.count = 1;
      base.first = u.whole + (2 * u.rem >= u.den ? 1 : 0);
      base.weight[0] = 1.0;
      break;
    case 2:
      base.count = 2;
      base.first = u.whole;
      base.weight[0] = 1.0 - phi;
      base.weight[1] = phi;
      break;
    case 4:
      base.count = 4;
      base.first = u.whole - 1;
      for (let t = 0; t < 4; t += 1) base.weight[t] = kernel(4, phi + 1.0 - t);
      break;
    case 8:
    default: {
      base.count = 8;
      base.first = u.whole - 3;
      if (u.rem === 0) {
        // On a line every other tap is sin( pi k ) / ( pi k ): 1e-17, not zero.
        base.weight[3] = 1.0;
        break;
      }
      let sum = 0.0;
      for (let t = 0; t < 8; t += 1) {
        base.weight[t] = kernel(8, phi + 3.0 - t);
        sum += base.weight[t];
      }
      for (let t = 0; t < 8; t += 1) base.weight[t] /= sum;
      break;
    }
  }

  if (softness <= 0.0) return base;

  // The older boxes' vertical pre-filter: [ a, 1 - 2a, a ] across the field's
  // own lines, convolved in front of the interpolator.
  const a = 0.25 * softness;
  const g = [a, 1.0 - 2.0 * a, a];
  const out = { first: base.first - 1, count: base.count + 2, weight: new Array(MAX_LINE_TAPS).fill(0) };
  for (let t = 0; t < base.count; t += 1) {
    for (let d = 0; d < 3; d += 1) out.weight[t + d] += base.weight[t] * g[d];
  }
  return out;
}

//===========================================================================
// Controls.cpp, ported.
//===========================================================================

const optionIndex = (value, count) => Math.min(count - 1, Math.max(0, Math.round(value)));
const tapsFromOption = (value) => TAP_COUNTS[optionIndex(value, TAP_OPTION_COUNT)];
const softnessFromParam = (value) => Math.min(1, Math.max(0, value));

const SHOW_WEAVE = 0;
const SHOW_COUNT = 2;
const OUTPUT_COUNT = 2;

//===========================================================================
// Schedule.cpp, ported: which fields happen on this host frame.
//===========================================================================

const NONE = Number.NEGATIVE_INFINITY;

class Schedule {
  constructor() {
    this.primed = false;
    this.prevTime = 0;
    this.nextSource = 0;
    this.newestSource = 0;
    this.nextDest = 0;
    this.latestDest = NONE;
  }

  reset() {
    this.primed = false;
  }

  static delay(c) {
    return (2.0 * c.source.fieldRate.den) / c.source.fieldRate.num;
  }

  /// Returns { captures: [{ field, fromCurrent }], primedNow }.
  frame(now, c) {
    const captures = [];
    const rs = c.source.fieldRate;
    const due = Math.floor(((now + Schedule.kEpsilon) * rs.num) / rs.den);

    if (!this.primed) {
      for (let n = due - Schedule.kPrimeFields + 1; n <= due; n += 1) captures.push({ field: n, fromCurrent: true });
      this.primed = true;
      this.prevTime = now;
      this.nextSource = due + 1;
      this.newestSource = due;
      this.nextDest = firstDestinationFrom(due - Schedule.kPrimeFields + 1, c);
      this.latestDest = NONE;
      return { captures, primedNow: true };
    }

    if (due - this.nextSource + 1 > Schedule.kMaxPerFrame) this.nextSource = due - Schedule.kMaxPerFrame + 1;

    for (; this.nextSource <= due; this.nextSource += 1) {
      const t = fieldTime(this.nextSource, rs);
      // Nearest; a tie goes to the earlier frame.
      const fromCurrent = now - t < t - this.prevTime - Schedule.kEpsilon;
      captures.push({ field: this.nextSource, fromCurrent });
      this.newestSource = this.nextSource;
    }

    this.prevTime = now;
    return { captures, primedNow: false };
  }

  /// Destination fields due by `now`, oldest first.
  destinations(now, c) {
    const out = [];
    if (!this.primed) return out;

    const rd = c.destination.fieldRate;
    const delay = Schedule.delay(c);
    const ready = (j) => fieldTime(j, rd) + delay <= now + Schedule.kEpsilon && requiredSource(j, c) <= this.newestSource;

    let last = this.nextDest - 1;
    while (ready(last + 1)) last += 1;
    if (last < this.nextDest) return out;

    if (last - this.nextDest + 1 > Schedule.kMaxPerFrame) this.nextDest = last - Schedule.kMaxPerFrame + 1;

    for (; this.nextDest <= last; this.nextDest += 1) out.push(this.nextDest);
    this.latestDest = last;
    return out;
  }

  hasDestination() {
    return this.latestDest !== NONE;
  }
}
Schedule.kPrimeFields = 7;
Schedule.kMaxPerFrame = 8;
Schedule.kEpsilon = 1e-7;

//===========================================================================
// Clock.cpp, ported, with the unit declared as seconds (SetScaleForTest(1),
// as sttest does). A delta is believed unless it is backwards or longer than
// half a second, in which case the clock steps on by one nominal frame.
//===========================================================================

class Clock {
  constructor() {
    this.started = false;
    this.lastScaled = -1;
    this.anchor = 0;
    this.offset = 0;
    this.now = 0;
    this.jumped = false;
  }

  update(scaled) {
    this.jumped = false;
    if (!this.started) {
      this.started = true;
      this.anchor = scaled;
      this.offset = 0;
      this.now = 0;
      this.lastScaled = scaled;
      return;
    }
    const delta = scaled - this.lastScaled;
    if (delta < 0 || delta > Clock.kMaxFrameSeconds) {
      this.jumped = true;
      this.offset = this.now + Clock.kNominalFrameSeconds;
      this.anchor = scaled;
      this.now = this.offset;
    } else {
      this.now = this.offset + (scaled - this.anchor);
    }
    this.lastScaled = scaled;
  }
}
Clock.kMaxFrameSeconds = 0.5;
Clock.kNominalFrameSeconds = 1.0 / 60.0;

//===========================================================================
// The renderer: Standards::ProcessOpenGL, in its order.
//
//   1. capture   this host frame onto the source standard's frame lines
//   2. field     source fields due since the last frame, each from the
//                nearer of this frame and the one before
//   3. convert   destination fields due, at their own instants — with the
//                motion pass first when Motion Comp is on
//   4. display   the latest pair (Weave) or field (Bob), onto the canvas
//===========================================================================

const K_SOURCE_RING = 8;
const K_DEST_RING = 4;

const blocksAcross = (width) => Math.max(1, Math.floor((width + K_BLOCK_WIDTH - 1) / K_BLOCK_WIDTH));
const blocksDown = (fieldLines) => Math.max(1, Math.floor((fieldLines + K_BLOCK_LINES - 1) / K_BLOCK_LINES));
const slotOf = (index, ring) => floorMod(index, ring);

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { ready: false };

function createRenderer(gl, quad) {
  const captureShader = new Program(gl, VERTEX, CAPTURE, 'capture');
  const fieldShader = new Program(gl, VERTEX, FIELD, 'field');
  const resampleShader = new Program(gl, VERTEX, RESAMPLE, 'resample');
  const motionSadShader = new Program(gl, VERTEX, MOTION_SAD, 'motion SAD');
  const motionPickShader = new Program(gl, VERTEX, MOTION_PICK, 'motion pick');
  const convertShader = new Program(gl, VERTEX, CONVERT, 'convert');
  const displayShader = new Program(gl, VERTEX, DISPLAY, 'display');

  // Half float for every field, as the plugin: an 8-bit code value survives
  // the round trip exactly and negative Catmull-Rom lobes survive.
  const FIELD_FORMAT = gl.RGBA16F;
  const nearest = () => new PassBuffer(gl, { filter: 'nearest' });

  const lines = [nearest(), nearest()];
  let linesPrevious = 0;
  const source = Array.from({ length: K_SOURCE_RING }, nearest);
  const sourceIndex = new Array(K_SOURCE_RING).fill(NONE);
  const destination = Array.from({ length: K_DEST_RING }, nearest);
  const destIndex = new Array(K_DEST_RING).fill(NONE);
  const costs = nearest();
  const vectors = nearest();
  let vectorsPair = 0;
  let vectorsValid = false;
  const scratch = nearest();

  const tableTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, tableTexture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  let tableKey = '';

  let storeWidth = 0;
  let haveConversion = false;
  let conversion = null;
  let conversionDirection = -1;

  const schedule = new Schedule();
  const clock = new Clock();

  const invalidateStore = () => {
    sourceIndex.fill(NONE);
    destIndex.fill(NONE);
    vectorsValid = false;
  };

  const ensureStore = (width, c) => {
    for (const b of lines) b.ensure(width, c.source.lines, FIELD_FORMAT);
    for (const b of source) b.ensure(width, c.source.fieldLines, FIELD_FORMAT);
    for (const b of destination) b.ensure(width, c.destination.fieldLines, FIELD_FORMAT);
    const across = 2 * K_SEARCH_X + 1;
    const down = 2 * K_SEARCH_LINES + 1;
    costs.ensure(blocksAcross(width) * across, blocksDown(c.source.fieldLines) * down, gl.RGBA32F);
    vectors.ensure(blocksAcross(width), blocksDown(c.source.fieldLines), gl.RGBA32F);
  };

  const resampleInto = (buffer, oldWidth, newWidth, height) => {
    // Old contents into the scratch buffer at the new width, then the slot
    // reallocated at the new width (which clears it) and the scratch copied
    // back. Two passes, once per resize, and the field survives.
    scratch.ensure(newWidth, height, FIELD_FORMAT);
    scratch.bind();
    resampleShader.use();
    bindTexture(gl, 0, buffer.texture);
    resampleShader.setSampler('Source', 0);
    resampleShader.setInt('OldWidth', oldWidth);
    resampleShader.setInt('NewWidth', newWidth);
    quad.draw();

    buffer.ensure(newWidth, height, FIELD_FORMAT);
    buffer.bind();
    bindTexture(gl, 0, scratch.texture);
    resampleShader.setSampler('Source', 0);
    resampleShader.setInt('OldWidth', newWidth);
    resampleShader.setInt('NewWidth', newWidth);
    quad.draw();
    bindTexture(gl, 0, null);
  };

  const resizeStore = (newWidth, c) => {
    resampleInto(lines[linesPrevious], storeWidth, newWidth, c.source.lines);
    for (let i = 0; i < K_SOURCE_RING; i += 1) {
      if (sourceIndex[i] !== NONE) resampleInto(source[i], storeWidth, newWidth, c.source.fieldLines);
    }
    for (let i = 0; i < K_DEST_RING; i += 1) {
      if (destIndex[i] !== NONE) resampleInto(destination[i], storeWidth, newWidth, c.destination.fieldLines);
    }
    scratch.dispose();
    vectorsValid = false;
  };

  const uploadTable = (c, taps, softness) => {
    const key = `${c.source.lines}/${c.destination.lines}/${taps}/${softness}/${c.destination.fieldLines}`;
    if (key === tableKey) return;
    tableKey = key;

    // Row ( q * 2 + p ) * Ld/2 + i: destination field line i of parity q, read
    // from a source field of parity p. Three RGBA texels: first line, tap
    // count, ten weights.
    const lfd = c.destination.fieldLines;
    const table = new Float32Array(4 * lfd * 12);
    for (let q = 0; q < 2; q += 1) {
      for (let p = 0; p < 2; p += 1) {
        for (let i = 0; i < lfd; i += 1) {
          const t = verticalTaps(2 * i + q, p, c, taps, softness);
          const row = ((q * 2 + p) * lfd + i) * 12;
          table[row] = t.first;
          table[row + 1] = t.count;
          for (let k = 0; k < t.count && k < MAX_LINE_TAPS; k += 1) table[row + 2 + k] = t.weight[k];
        }
      }
    }
    gl.bindTexture(gl.TEXTURE_2D, tableTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 3, 4 * lfd, 0, gl.RGBA, gl.FLOAT, table);
    gl.bindTexture(gl.TEXTURE_2D, null);
  };

  const sourceTexture = (field) => {
    const slot = slotOf(field, K_SOURCE_RING);
    if (sourceIndex[slot] === field) return source[slot].texture;
    // Not in the store: the nearest field that is there is the least wrong
    // picture; black would be a hole.
    let best = -1;
    let bestDelta = Infinity;
    for (let i = 0; i < K_SOURCE_RING; i += 1) {
      if (sourceIndex[i] === NONE) continue;
      const d = Math.abs(sourceIndex[i] - field);
      if (d < bestDelta) {
        bestDelta = d;
        best = i;
      }
    }
    return best >= 0 ? source[best].texture : null;
  };

  const destinationTexture = (field) => {
    const slot = slotOf(field, K_DEST_RING);
    if (destIndex[slot] === field) return destination[slot].texture;
    const latest = schedule.latestDest;
    const latestSlot = slotOf(latest, K_DEST_RING);
    return destIndex[latestSlot] === latest ? destination[latestSlot].texture : null;
  };

  const captureFrame = (picture, frameLines) => {
    const target = lines[1 - linesPrevious];
    target.bind();
    captureShader.use();
    bindTexture(gl, 0, picture.texture);
    captureShader.setSampler('InputTexture', 0);
    captureShader.setInt('InWidth', picture.width);
    captureShader.setInt('InHeight', picture.height);
    captureShader.setInt('Width', storeWidth);
    captureShader.setInt('Lines', frameLines);
    quad.draw();
  };

  const makeField = (field, fromCurrent) => {
    const slot = slotOf(field, K_SOURCE_RING);
    const input = fromCurrent ? lines[1 - linesPrevious] : lines[linesPrevious];
    source[slot].bind();
    fieldShader.use();
    bindTexture(gl, 0, input.texture);
    fieldShader.setSampler('FrameLines', 0);
    fieldShader.setInt('Parity', fieldParity(field));
    quad.draw();
    bindTexture(gl, 0, null);
    sourceIndex[slot] = field;
  };

  const estimateMotion = (pair) => {
    if (vectorsValid && vectorsPair === pair) return;

    costs.bind();
    motionSadShader.use();
    bindTexture(gl, 0, sourceTexture(pair));
    bindTexture(gl, 1, sourceTexture(pair + 1));
    motionSadShader.setSampler('FieldA', 0);
    motionSadShader.setSampler('FieldB', 1);
    motionSadShader.setInt('ParityA', fieldParity(pair));
    motionSadShader.setInt('ParityB', fieldParity(pair + 1));
    motionSadShader.setInt('Width', storeWidth);
    motionSadShader.setInt('FieldLines', conversion.source.fieldLines);
    motionSadShader.setInt('BlockWidth', K_BLOCK_WIDTH);
    motionSadShader.setInt('BlockLines', K_BLOCK_LINES);
    motionSadShader.setInt('SearchX', K_SEARCH_X);
    motionSadShader.setInt('SearchLines', K_SEARCH_LINES);
    motionSadShader.set('Lambda', 1e-3);
    quad.draw();

    vectors.bind();
    motionPickShader.use();
    bindTexture(gl, 0, costs.texture);
    bindTexture(gl, 1, null);
    motionPickShader.setSampler('Costs', 0);
    motionPickShader.setInt('SearchX', K_SEARCH_X);
    motionPickShader.setInt('SearchLines', K_SEARCH_LINES);
    quad.draw();
    bindTexture(gl, 0, null);

    vectorsPair = pair;
    vectorsValid = true;
  };

  const convert = (j, c, temporal, motion) => {
    const taps = temporalWeights(j, c, temporal);
    const q = fieldParity(j);
    const lfd = c.destination.fieldLines;

    if (motion) estimateMotion(Math.floor(taps.position));

    const rows = [0, 0, 0, 0];
    const weights = [0, 0, 0, 0];
    const offsets = [0, 0, 0, 0];
    const textures = [null, null, null, null];
    for (let k = 0; k < taps.count; k += 1) {
      const n = taps.first + k;
      rows[k] = (q * 2 + fieldParity(n)) * lfd;
      weights[k] = taps.weight[k];
      offsets[k] = n - taps.position;
      textures[k] = sourceTexture(n);
    }
    // Unused samplers still need a texture bound: the field that is there.
    for (let k = taps.count; k < 4; k += 1) textures[k] = textures[0];

    const slot = slotOf(j, K_DEST_RING);
    destination[slot].bind();
    convertShader.use();
    for (let k = 0; k < 4; k += 1) bindTexture(gl, k, textures[k]);
    bindTexture(gl, 4, tableTexture);
    bindTexture(gl, 5, vectors.texture);

    convertShader.setSampler('Field0', 0);
    convertShader.setSampler('Field1', 1);
    convertShader.setSampler('Field2', 2);
    convertShader.setSampler('Field3', 3);
    convertShader.setSampler('Table', 4);
    convertShader.setSampler('Vectors', 5);
    convertShader.setInt('Row0', rows[0]);
    convertShader.setInt('Row1', rows[1]);
    convertShader.setInt('Row2', rows[2]);
    convertShader.setInt('Row3', rows[3]);
    convertShader.setInt('Count', taps.count);
    convertShader.set('Weights', weights[0], weights[1], weights[2], weights[3]);
    convertShader.set('Offsets', offsets[0], offsets[1], offsets[2], offsets[3]);
    convertShader.setInt('Width', storeWidth);
    convertShader.setInt('SourceLines', c.source.fieldLines);
    convertShader.setInt('DestLines', lfd);
    convertShader.setInt('UseMC', motion ? 1 : 0);
    convertShader.setInt('BlocksX', blocksAcross(storeWidth));
    convertShader.setInt('BlocksY', blocksDown(c.source.fieldLines));
    convertShader.setInt('BlockWidth', K_BLOCK_WIDTH);
    convertShader.setInt('BlockLines', K_BLOCK_LINES);
    convertShader.set('VectorSign', 1.0);
    quad.draw();
    for (let k = 0; k < 6; k += 1) bindTexture(gl, k, null);

    destIndex[slot] = j;
    return taps;
  };

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      clock.update(time);
      const now = clock.now;
      const p = (id) => params.get(id);

      const direction = optionIndex(p('direction'), DIRECTION_COUNT);
      const temporal = optionIndex(p('temporal'), TEMPORAL_COUNT);
      const taps = tapsFromOption(p('taps'));
      const motion = p('motion') >= 0.5;
      const showAs = optionIndex(p('showAs'), SHOW_COUNT);
      const outputSize = optionIndex(p('outputSize'), OUTPUT_COUNT);
      const soft = softnessFromParam(p('softness'));

      const picture = input;
      const width = picture.width;
      gl.disable(gl.BLEND);

      // A change of direction is a different machine: the store's line
      // structure changes, so it is emptied and primed again.
      if (!haveConversion || direction !== conversionDirection) {
        conversion = conversionOf(direction);
        conversionDirection = direction;
        haveConversion = true;
        schedule.reset();
        invalidateStore();
      }
      const c = conversion;

      // A change of width keeps the store: every field is carried across.
      if (storeWidth !== 0 && width !== storeWidth && schedule.primed) resizeStore(width, c);
      ensureStore(width, c);
      storeWidth = width;
      uploadTable(c, taps, soft);

      // 1. This host frame onto the source lines.
      captureFrame(picture, c.source.lines);

      // 2. Source fields due since the last frame.
      const { captures, primedNow } = schedule.frame(now, c);
      if (primedNow) invalidateStore();
      for (const capture of captures) makeField(capture.field, capture.fromCurrent);
      linesPrevious = 1 - linesPrevious;

      // 3. Destination fields due, at their own instants.
      const due = schedule.destinations(now, c);
      for (const j of due) {
        const weights = convert(j, c, temporal, motion);
        telemetry.weights = weights;
      }

      if (!schedule.hasDestination()) return;

      // 4. Display. Weave: the latest complete pair, one field of latency.
      //    Bob: the latest field.
      const latest = schedule.latestDest;
      let older = latest;
      let newer = latest;
      if (showAs === SHOW_WEAVE) {
        if (fieldParity(latest) === 1) {
          older = latest - 1;
          newer = latest;
        } else {
          older = latest - 2;
          newer = latest - 1;
        }
      }

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);

      displayShader.use();
      bindTexture(gl, 0, destinationTexture(older));
      bindTexture(gl, 1, destinationTexture(newer));
      bindTexture(gl, 2, picture.texture);
      displayShader.setSampler('FieldA', 0);
      displayShader.setSampler('FieldB', 1);
      displayShader.setSampler('InputTexture', 2);
      displayShader.setInt('ParityA', fieldParity(older));
      displayShader.setInt('ParityB', fieldParity(newer));
      displayShader.setInt('ShowAs', showAs);
      displayShader.setInt('OutputSize', outputSize);
      displayShader.setInt('NativeOffset', floorDiv(vpH - c.destination.lines, 2));
      displayShader.setInt('VpX', 0);
      displayShader.setInt('VpY', 0);
      displayShader.setInt('VpW', vpW);
      displayShader.setInt('VpH', vpH);
      displayShader.setInt('Width', storeWidth);
      displayShader.setInt('FrameLines', c.destination.lines);
      displayShader.setInt('FieldLines', c.destination.fieldLines);
      // The kit's clip fills its texture, so the whole texture is picture.
      displayShader.set('MaxUV', 1, 1);
      displayShader.set('MixAmount', p('mix'));
      quad.draw();
      for (let k = 0; k < 3; k += 1) bindTexture(gl, k, null);
      gl.activeTexture(gl.TEXTURE0);

      telemetry.ready = true;
      telemetry.conversion = `${c.source.name} > ${c.destination.name}`;
      telemetry.newestSource = schedule.newestSource;
      telemetry.latest = latest;
      telemetry.pair = showAs === SHOW_WEAVE ? `${older}+${newer}` : `${latest}`;
    },
  };
}

//===========================================================================
// The controls, read out of Standards::Standards(). Same names, same groups,
// same order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const demo = mountDemo({
  name: 'Standards',
  pluginId: 'ST01',
  tagline:
    'A field-store standards converter: 625/50 to 525/59.94 and back, the way programmes crossed the Atlantic before the 1990s. The clip becomes fields of the source standard at their own instants; each output field is interpolated in time from the source fields either side of it and each output line from nearby lines of the field being read. Neither knows about motion, so the look falls out — a judder that repeats every six fields and creeps at 59.94, double images, a soft picture. The shaders here are the plugin’s own; the field schedule and the interpolation tables are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/standards',

  // The stock sentence says "same maths", which is only half true here: the
  // shaders are the plugin's, the schedule and tables around them are a port.
  blurb:
    'It is Standards’ own GLSL, ported from the repository to WebGL2, with the CPU half that decides which fields exist and how they are weighted ported to JavaScript by hand — nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // The field store is RGBA16F and the motion costs RGBA32F, as in the plugin.
  needFloat: true,

  params: [
    opt('direction', 'Direction', ['625/50 > 525/59.94', '525/59.94 > 625/50', '625/50 > 525/60', 'Same (no conversion)'], 0, 'Conversion',
      'Which way across the Atlantic. 50 into 60 is 5 into 6, so the weights repeat every six output fields; at 59.94 the phase moves on by 1/200 of a field each cycle — the creep. Same is the field store and nothing else.'),
    opt('temporal', 'Temporal', ['Drop/Repeat', 'Linear', 'Four Field'], 1, 'Conversion',
      'How each output field is made from the source fields around its instant: the nearest one, the two either side weighted by distance, or four with Catmull-Rom weights in time.'),
    opt('taps', 'Vertical Taps', ['1', '2', '4', '8'], 2, 'Conversion',
      'Lines of the source field read for each output line: nearest, linear, Catmull-Rom, or Lanczos-4. The taps only read lines that exist in the field being read — that is what respecting field parity means.'),
    bool('motion', 'Motion Comp', 0, 'Conversion',
      'Crude block vectors between each pair of source fields — integer pixels, integer field lines, one vector per 16 × 8 block, no smoothing — which smooth a pan and tear at an occlusion, the way the first motion-compensated converters did.'),

    opt('showAs', 'Show As', ['Weave', 'Bob'], 0, 'Display',
      'Weave shows the latest complete pair, each field on its own lines, as an interlaced monitor does. Bob shows the latest field on its own lines and averages them into the others.'),
    opt('outputSize', 'Output Size', ['Native Lines', 'Host'], 1, 'Display',
      'Host scales the destination frame to the composition’s height. Native Lines draws one row per destination line, centred, with black outside.'),

    std('softness', 'Softness', 0.25, 'Look', {
      display: (v) => {
        const a = 0.25 * softnessFromParam(v);
        return a === 0 ? 'off' : `[${a.toFixed(3)}, ${(1 - 2 * a).toFixed(3)}, ${a.toFixed(3)}]`;
      },
      hint: 'The older boxes’ vertical pre-filter, [ a, 1 − 2a, a ] across the field’s own lines with a a quarter of Softness, convolved in front of the interpolator.',
    }),
    std('mix', 'Mix', 1.0, 'Look'),
  ],

  // Something has to move for a converter to show anything. The geometry card
  // rotates, so its spokes and ring edges move fastest at the rim; the lights
  // cross the frame; the scene drifts.
  sources: ['grid', 'spot', 'scene', 'ramp', 'detail', 'bars'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Drop/Repeat, one tap (the crudest box)': { temporal: 0, taps: 0, softness: 0 },
    'Four Field, eight taps (the best box)': { temporal: 2, taps: 3, softness: 0 },
    'Motion compensated': { motion: 1, temporal: 2 },
    'NTSC to PAL': { direction: 1 },
    'Exactly 60 (no creep)': { direction: 2 },
    'Bob, native lines': { showAs: 1, outputSize: 0 },
    'Field store only': { direction: 3, temporal: 0, taps: 0, softness: 0 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Standards decides on the CPU, in exact rationals, when every field is, which source fields and weights make each output field, and where every output line falls among a source field’s lines — Model.cpp, Schedule.cpp, Controls.cpp and the store bookkeeping in Standards::ProcessOpenGL. All of it is ported here function for function, because without it the shaders have nothing to be told. Nothing checks a port but a reader; the repository’s sttest checks the C++ and has never heard of this page.',
    'The GPU half is not a port. The capture, field, resample, motion, convert and display passes are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the eight shaders drifts. The field store is RGBA16F and the motion costs RGBA32F, as in the plugin.',
    'Fields run on the page’s clock with its unit declared as seconds, as the repository’s harness declares it; the plugin votes on Resolume’s clock unit and that vote never runs here. The browser paints at your display’s rate, which is not locked to either standard, so where the source fields fall between painted frames depends on this machine — as it depends on Resolume’s frame rate in the plugin. Step advances the clock by exactly 1/60 s, which is the way to watch the six-field cycle one field at a time.',
    'The generated clips move slowly, and a converter shows most on fast motion. The geometry card’s rim is the fastest thing on offer; your own footage of a pan (Use my own…) shows the judder and the double images far better.',
    'There is no audio caveat on this page: Standards has no audio path.',
    'The plugin’s numerical proof — every output field’s levels against its temporal weights, the six-field cycle bit for bit, the 1/200 creep, marked lines landing where the geometry says, motion compensation tearing only where an occlusion reaches — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas: the ported schedule's own numbers. Which output
// field is on show, which source fields made it and at what weights. Step
// through six fields and the weights repeat — or, at 59.94, very nearly do.
// Skipped in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      if (!telemetry.ready || !telemetry.weights) return;
      const w = telemetry.weights;
      const fields = [];
      for (let k = 0; k < w.count; k += 1) fields.push(`${w.first + k} × ${w.weight[k].toFixed(3)}`);
      line.textContent =
        `${telemetry.conversion}: output field ${telemetry.latest.toLocaleString('en-GB')} on show (${telemetry.pair}), `
        + `made at source position ${w.position.toFixed(3)} from ${fields.join(' + ')}. `
        + `Newest source field ${telemetry.newestSource.toLocaleString('en-GB')}.`;
    }, 250);
  }
}
