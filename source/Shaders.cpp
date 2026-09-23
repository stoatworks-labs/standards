#include "Shaders.h"

namespace standards::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// capture: the host's picture onto the source standard's frame lines.
//
// Frame line l (top first) takes host row floor( ( 2l + 1 ) H / ( 2 Ls ) ):
// the row under the line's centre, as a camera's line samples the scene
// under it. Integer arithmetic, so the row it picks is the same on every
// rasteriser; at H = Ls it is row l exactly. The width is the host's.
//---------------------------------------------------------------------------
const char* const kCapture = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// field: one field's lines out of a frame's. Field line k of parity p is
// frame line 2k + p.
//---------------------------------------------------------------------------
const char* const kField = R"(#version 410 core

uniform sampler2D FrameLines;
uniform int Parity;

out vec4 fragColor;

void main()
{
	int x = int( gl_FragCoord.x );
	int k = int( gl_FragCoord.y );
	fragColor = texelFetch( FrameLines, ivec2( x, 2 * k + Parity ), 0 );
}
)";

//---------------------------------------------------------------------------
// resample: a buffer at a new width, nearest column. Only ever runs on a
// resize, so that the fields already in the store survive it (a store that
// cleared on a resize would show black, or the wrong picture, for the whole
// aperture -- photofinish's bug).
//---------------------------------------------------------------------------
const char* const kResample = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// motion, in two passes: one vector per block, from field A (source field n)
// to field B (n + 1), by exhaustive SAD on luma. Crude on purpose -- integer
// pixels, integer field lines, one vector per block, no smoothing, no
// occlusion reasoning -- because that is what the first motion-compensated
// converters were, and their failures are the point.
//
// The search is spread across fragments, not looped inside one: the first
// pass writes one SAD per (block, candidate), the second picks each block's
// least. A single fragment per block looping over all 165 candidates made
// the GPU wait on a few thousand long serial loops -- 41 ms a frame at every
// raster, found by the bench.
//
// A and B have opposite parity, so B's lines sit half a line away from A's.
// B is read AT A's line positions by averaging the two lines either side:
// the one place a half-line average is right, because it is a comparison and
// not a picture anybody sees.
//---------------------------------------------------------------------------
const char* const kMotionSad = R"(#version 410 core

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
		for( int xx = 0; xx < BlockWidth; xx += 2 )
			sad += abs( lumaAt( FieldA, x0 + xx, k0 + yy ) - bAtLineOfA( x0 + xx + dx, k0 + yy + dy ) );

	fragColor = vec4( sad + Lambda * float( abs( dx ) + abs( dy ) ), 0.0, 0.0, 1.0 );
}
)";

const char* const kMotionPick = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// convert: one destination field from one, two or four source fields.
//
// In space, each source field is filtered onto the destination line by a
// table the CPU built from exact line positions (Model.h VerticalTaps): row
// Row<k> + i holds the first field line, the tap count and up to ten
// weights, for destination field line i against source field k's parity.
// The taps read only lines that exist in field k -- that is what respecting
// field parity means.
//
// In time, the filtered fields are summed with the temporal weights, also
// from the CPU. Motion compensation, when on, fetches each field along the
// block's vector at that field's distance from the destination instant:
// horizontally to a fraction of a pixel, vertically to the nearest field
// line (crude, and said so).
//
// Every sum starts at zero and adds weight x sample, so one tap of weight 1
// returns the sample bit for bit, and a weight of 0 contributes nothing.
//---------------------------------------------------------------------------
const char* const kConvert = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// display: destination fields onto the host's framebuffer.
//
// Weave shows the latest complete pair, each field on its own lines, as an
// interlaced monitor does over a frame period. Bob shows the latest field on
// its own lines and averages its lines either side into the others --
// cadence's Bob Linear.
//
// Output row Y (top first) shows destination frame line m: scaled to the
// host's height (floor( ( 2Y + 1 ) Ld / ( 2 H ) )), or one row per line,
// centred (Y - Offset), with black outside. Integer arithmetic from
// gl_FragCoord and the host's viewport, never from an interpolated uv, so
// the line a row shows is the same on every rasteriser.
//---------------------------------------------------------------------------
const char* const kDisplay = R"(#version 410 core

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
)";

} // namespace standards::shaders
