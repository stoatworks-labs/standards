/**
	sttest -- render Standards offline, and read the conversion back out of it.

	Where a destination field falls between two source fields, which source
	lines a destination line is made from, and what a converter does to
	something moving are facts with one right answer each. Every check here
	drives the REAL plugin class through a headless GL context and measures
	the answer out of the picture it made:

		sttest --out /tmp/frame.png     a picture, on the moving test card
		sttest --list                   every parameter, its kind and default
		sttest --weights                the input encodes each source field's
		                                number as levels; each output field's
		                                levels ARE its temporal weights. 50 -> 60
		                                repeats exactly every six fields; 50 ->
		                                59.94 creeps 0.005 of a field per cycle
		sttest --judder                 a bar moving v px per source field: the
		                                repeat pattern under Drop/Repeat, the
		                                double image's two components under
		                                Linear, whole-pixel and fractional
		sttest --lines                  a marked line lands where the geometry
		                                says; the impulse response and a zone
		                                plate's measured response are the chosen
		                                taps' transfer function
		sttest --same                   Same, Drop/Repeat, one tap: the output
		                                is the fielded input, bit for bit
		sttest --mc                     motion compensation removes the judder of
		                                a pan and tears where an occlusion says
		sttest --resize                 a resize mid-run keeps the field store
		sttest --negative               every check above can FAIL
		sttest --offline                the checks that need no GL (the model's
		                                arithmetic, a six-day clock, the names)
		sttest --bench                  the render cost
		sttest --dump-shaders DIR       the exact GLSL the plugin compiles
		sttest --pipe                   raw frames in, raw frames out

	The harness places field instants exactly by driving SetTime at a high
	synthetic rate: 600 frames a second against a 50-field source, 600000/1001
	against 59.94, so every source field's instant IS a host frame. Every
	check takes --size; run each at the raster you care about and at 320x180,
	which is CI's. AGENTS.md has one line per check on where each tolerance
	comes from.

	The standards, the kernels and the time geometry the checks predict from
	are stated HERE, from the definitions, and never read out of Model.h: a
	constant typed wrong there has to show up as a failed check, not as an
	agreement.
*/

#include "Clock.h"
#include "Model.h"
#include "Schedule.h"
#include "Shaders.h"
#include "Standards.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = standards::model;

int g_checks   = 0;
int g_failures = 0;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The standards and the geometry, stated from the definitions.
//---------------------------------------------------------------------------
struct Rate
{
	int64_t num;
	int64_t den;
};

struct Std
{
	int lines;
	Rate rate;
};

const Std k625 = { 576, { 50, 1 } };
const Std k525 = { 480, { 60000, 1001 } };
const Std k525At60 = { 480, { 60, 1 } };

struct Dir
{
	const char* name;
	int option;
	Std src;
	Std dst;
};

const Dir kDir625To525     = { "625/50 > 525/59.94", 0, k625, k525 };
const Dir kDir525To625     = { "525/59.94 > 625/50", 1, k525, k625 };
const Dir kDir625To525At60 = { "625/50 > 525/60", 2, k625, k525At60 };
const Dir kDirSame         = { "Same", 3, k625, k625 };

int64_t floorDiv( int64_t a, int64_t b )
{
	int64_t q = a / b;
	if( ( a % b ) != 0 && ( a < 0 ) )
		--q;
	return q;
}

int parityOf( int64_t k )
{
	return static_cast< int >( ( ( k % 2 ) + 2 ) % 2 );
}

/// p = j / Rd * Rs as n + rem / den, exactly.
struct Frac
{
	int64_t n;
	int64_t rem;
	int64_t den;
	double phi() const
	{
		return static_cast< double >( rem ) / static_cast< double >( den );
	}
};

Frac positionOf( int64_t j, const Dir& d )
{
	//Field j of the destination is at t = j Rd.den / Rd.num seconds; in
	//source fields that is t Rs.num / Rs.den.
	const int64_t num = d.dst.rate.den * d.src.rate.num;
	const int64_t den = d.dst.rate.num * d.src.rate.den;
	Frac f;
	f.den = den;
	f.n   = floorDiv( j * num, den );
	f.rem = j * num - f.n * den;
	return f;
}

/// Destination frame line m's centre, in field-line units of a source field of
/// parity p: the line's centre is at ( m + 1/2 ) / Ld of the height, the
/// source's frame line l at ( l + 1/2 ) / Ls, and field line k of parity p is
/// frame line 2k + p.
Frac sourceLineOf( int m, int p, const Dir& d )
{
	const int64_t ls = d.src.lines, ld = d.dst.lines;
	//( ( m + 1/2 ) Ls / Ld - 1/2 - p ) / 2, over a common denominator.
	const int64_t num = ( 2 * static_cast< int64_t >( m ) + 1 ) * ls - ld - 2 * p * ld;
	const int64_t den = 4 * ld;
	Frac f;
	f.den = den;
	f.n   = floorDiv( num, den );
	f.rem = num - f.n * den;
	return f;
}

double sinc( double x )
{
	return x == 0.0 ? 1.0 : std::sin( M_PI * x ) / ( M_PI * x );
}

/// The stated kernels, as TAPS on field lines first .. first + count - 1 for
/// a position whose floor is n and fraction t. Catmull-Rom is written in its
/// Hermite matrix form here, not as Keys' piecewise cubic: two statements of
/// one interpolator, so a typo in either does not agree with itself.
struct Taps
{
	int64_t first;
	int count;
	double w[ 8 ];
};

Taps statedTaps( int taps, const Frac& u )
{
	const double t = u.phi();
	Taps k         = {};
	switch( taps )
	{
	case 1:
		k.count  = 1;
		k.first  = u.n + ( 2 * u.rem >= u.den ? 1 : 0 );
		k.w[ 0 ] = 1.0;
		break;
	case 2:
		k.count  = 2;
		k.first  = u.n;
		k.w[ 0 ] = 1.0 - t;
		k.w[ 1 ] = t;
		break;
	case 4:
		k.count  = 4;
		k.first  = u.n - 1;
		k.w[ 0 ] = 0.5 * ( -t * t * t + 2 * t * t - t );
		k.w[ 1 ] = 0.5 * ( 3 * t * t * t - 5 * t * t + 2 );
		k.w[ 2 ] = 0.5 * ( -3 * t * t * t + 4 * t * t + t );
		k.w[ 3 ] = 0.5 * ( t * t * t - t * t );
		break;
	default:
	{
		k.count = 8;
		k.first = u.n - 3;
		double sum = 0.0;
		for( int i = 0; i < 8; ++i )
		{
			const double x = t + 3 - i;
			k.w[ i ]       = u.rem == 0 ? ( i == 3 ? 1.0 : 0.0 ) : sinc( x ) * sinc( x / 4.0 );
			sum += k.w[ i ];
		}
		for( int i = 0; i < 8; ++i )
			k.w[ i ] /= sum;
		break;
	}
	}
	return k;
}

/// The temporal aperture the spec states, for destination field j.
Taps statedTemporal( int temporal, const Frac& p )
{
	if( temporal == model::kDropRepeat )
	{
		Taps k   = {};
		k.count  = 1;
		k.first  = p.n + ( 2 * p.rem > p.den ? 1 : 0 );//ties repeat the earlier
		k.w[ 0 ] = 1.0;
		return k;
	}
	if( temporal == model::kLinear )
		return statedTaps( 2, { p.n, p.rem, p.den } );
	return statedTaps( 4, { p.n, p.rem, p.den } );
}

/// One unit in the last place of an IEEE half at |v|: the store is RGBA16F.
/// A full ULP rather than half of one, because GL lets a conversion to a
/// narrower float round either way.
double halfUlp( double v )
{
	v = std::fabs( v );
	if( v < 6.103515625e-05 )
		return 5.960464477539063e-08;//the subnormal step
	return std::ldexp( 1.0, static_cast< int >( std::floor( std::log2( v ) ) ) - 10 );
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Standards::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Standards& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Standards::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Standards& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Standards& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Standards& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
// The clock is a RATIONAL rate, so that frame k lands on k den / num seconds
// and a source field's instant can be a host frame exactly.
//---------------------------------------------------------------------------
struct Session
{
	Standards plugin;
	int width      = 0;
	int height     = 0;
	int64_t fpsNum = 60;
	int64_t fpsDen = 1;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip changes size: the SAME instance handed
	/// a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	double timeOf( int64_t frame ) const
	{
		return static_cast< double >( frame ) * static_cast< double >( fpsDen ) / static_cast< double >( fpsNum );
	}

	bool renderAt( int64_t frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( timeOf( frame ) );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %lld\n", static_cast< long long >( frame ) );
		return ok;
	}

	bool render( int64_t frame, const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	bool render( int64_t frame, const std::vector< float >& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// One output row, top-first index y, as floats.
	std::vector< float > readRow( int y )
	{
		std::vector< float > row( static_cast< size_t >( width ) * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, height - 1 - y, width, 1, GL_RGBA, GL_FLOAT, row.data() );
		return row;
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// Everything a check starts from: no pre-filter, one tap, the nearest
/// field, Bob onto the host's raster, no mix. Each check moves what it is
/// about.
void baseline( Standards& p, const Dir& d, int temporal )
{
	set( p, "Direction", static_cast< float >( d.option ) );
	set( p, "Temporal", static_cast< float >( temporal ) );
	set( p, "Vertical Taps", 0.0f );
	set( p, "Motion Comp", 0.0f );
	set( p, "Show As", 1.0f );
	set( p, "Output Size", 1.0f );
	set( p, "Softness", 0.0f );
	set( p, "Mix", 1.0f );
}

/// The host rate that puts every source field on a host frame: M frames a
/// field, M = 12 for 50 and 10 for 59.94 -- 600 and 599.4 frames a second.
void placeFieldsExactly( Session& s, const Dir& d, int64_t& framesPerField )
{
	framesPerField = d.src.rate.den == 1 ? 12 : 10;
	s.fpsNum       = d.src.rate.num * framesPerField;
	s.fpsDen       = d.src.rate.den;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( !quiet )
	{
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "  %s\n", verdict( ok ) );
	}
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --weights
//
// Every host frame that becomes a source field is painted with that field's
// number: the left third one-hot in RGBA on n mod 4, the middle third a level
// n / 1024. A destination field made from fields n..n+3 therefore carries,
// in channel c of the left third, exactly the weight of the one field in its
// aperture with n = c mod 4 -- the weights read straight out of the picture,
// with no assumption about which fields were used. The middle third says
// which n they were.
//---------------------------------------------------------------------------
std::vector< float > weightsFrame( int width, int height, int64_t field )
{
	std::vector< float > img( static_cast< size_t >( width ) * height * 4, 0.0f );
	const int64_t residue = ( ( field % 4 ) + 4 ) % 4;
	const float level     = static_cast< float >( field ) / 1024.0f;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			float* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			if( x < width / 3 )
				for( int c = 0; c < 4; ++c )
					px[ c ] = c == residue ? 1.0f : 0.0f;
			else
			{
				px[ 0 ] = px[ 1 ] = px[ 2 ] = level;
				px[ 3 ]                     = 1.0f;
			}
		}
	return img;
}

struct WeightReading
{
	int64_t j;
	float onehot[ 4 ];
	float level;
};

/// Run a conversion with the field-number card at a host rate that puts
/// every source field on a host frame, and read each destination field's
/// levels the first time it is shown. `resizeAt` > 0 doubles the raster at
/// that host frame.
bool readWeights( int width, int height, const Dir& d, int temporal, int perturb, int fields,
                  std::vector< WeightReading >& out, int64_t resizeAt = -1, int64_t* firstAfterResize = nullptr )
{
	Session s;
	baseline( s.plugin, d, temporal );
	s.plugin.SetPerturbForTest( perturb );
	int64_t m = 0;
	placeFieldsExactly( s, d, m );
	if( !s.begin( width, height ) )
		return false;

	out.clear();
	int64_t lastShown = INT64_MIN;
	std::vector< float > frame;
	int64_t frameField = INT64_MIN;
	for( int64_t k = 0; static_cast< int >( out.size() ) < fields && k < 200000; ++k )
	{
		if( k == resizeAt )
		{
			s.resize( s.width * 2, s.height * 2 );
			frameField = INT64_MIN;
			if( firstAfterResize )
				*firstAfterResize = INT64_MIN;
		}
		//The field this frame will be captured as, if any: frame k is field
		//k / M's instant exactly when M divides k. The others are never
		//nearest to any field, and carry the nearest field's number anyway.
		const int64_t field = floorDiv( 2 * k + m, 2 * m );
		bool ok             = true;
		if( field != frameField )
		{
			//Uploaded only when it changes: the plugin sees the same picture
			//either way, and at 720p the upload is most of the run's time.
			frame      = weightsFrame( s.width, s.height, field );
			frameField = field;
			ok         = s.render( k, frame );
		}
		else
			ok = s.renderAt( k );
		if( !ok )
		{
			s.end();
			return false;
		}

		int64_t older = 0, newer = 0;
		s.plugin.LastDisplayForTest( older, newer );
		if( newer == lastShown )
			continue;
		if( lastShown != INT64_MIN && newer != lastShown + 1 )
		{
			std::printf( "   the display skipped from field %lld to %lld\n", static_cast< long long >( lastShown ),
			             static_cast< long long >( newer ) );
			s.end();
			return false;
		}
		lastShown = newer;
		if( firstAfterResize && resizeAt >= 0 && k >= resizeAt && *firstAfterResize == INT64_MIN )
			*firstAfterResize = newer;

		const std::vector< float > row = s.readRow( s.height / 2 );
		WeightReading r;
		r.j = newer;
		for( int c = 0; c < 4; ++c )
			r.onehot[ c ] = row[ static_cast< size_t >( s.width / 6 ) * 4 + c ];
		r.level = row[ static_cast< size_t >( s.width * 2 / 3 ) * 4 ];
		out.push_back( r );
	}
	s.end();
	return static_cast< int >( out.size() ) >= fields;
}

/// Measured weights by field, out of one reading, given the aperture's first
/// field (read from the level, so nothing here trusts the plugin's own
/// bookkeeping): w[ k ] is field first + k's.
bool measuredAperture( const WeightReading& r, int count, const Taps& stated, double& worst, std::string& why )
{
	//The level is the weighted mean field number; the aperture it names has
	//to be the stated one. Resolution n / 1024 in half is 1/8 of a field.
	double mean = 0.0;
	for( int k = 0; k < stated.count; ++k )
		mean += stated.w[ k ] * static_cast< double >( stated.first + k );
	const double measuredMean = static_cast< double >( r.level ) * 1024.0;
	if( std::fabs( measuredMean - mean ) > 0.25 )
	{
		char b[ 160 ];
		std::snprintf( b, sizeof( b ), "mean field %.3f, stated %.3f", measuredMean, mean );
		why = b;
		return false;
	}

	worst = 0.0;
	double expected[ 4 ] = { 0, 0, 0, 0 };
	for( int k = 0; k < stated.count; ++k )
		expected[ ( ( stated.first + k ) % 4 + 4 ) % 4 ] += stated.w[ k ];
	for( int c = 0; c < 4; ++c )
	{
		const double err = std::fabs( static_cast< double >( r.onehot[ c ] ) - expected[ c ] );
		//One half-ULP of the store at the weight, plus the weight's own
		//rounding to float on the way into the uniform.
		const double tol = halfUlp( expected[ c ] ) + 1e-7;
		if( err > tol )
		{
			char b[ 200 ];
			std::snprintf( b, sizeof( b ), "channel %d weight %.6f, stated %.6f (tolerance %.1e)", c, r.onehot[ c ], expected[ c ], tol );
			why = b;
			return false;
		}
		worst = std::max( worst, err );
	}
	( void )count;
	return true;
}

int runWeights( int width, int height, int perturb = 0, bool quiet = false, bool creepOnly = false )
{
	int failures = 0;
	const int cycles = 20;
	const int want   = 6 * cycles + 12;

	struct Case
	{
		const Dir* d;
		int temporal;
	};
	std::vector< Case > cases = { { &kDir625To525At60, model::kLinear },  { &kDir625To525At60, model::kFourField },
		                          { &kDir625To525, model::kLinear },      { &kDir625To525, model::kFourField },
		                          { &kDir525To625, model::kLinear },      { &kDirSame, model::kLinear } };
	if( creepOnly )
		cases = { { &kDir625To525, model::kLinear } };

	for( const Case& cs : cases )
	{
		const Dir& d = *cs.d;
		std::vector< WeightReading > readings;
		if( !readWeights( width, height, d, cs.temporal, perturb, want, readings ) )
		{
			failures += report( false, quiet, "weights %s: could not render", d.name );
			continue;
		}

		//Every destination field whose whole aperture is made of fields
		//captured from their own frames (the priming fields all carry field
		//0's number, so any aperture reaching below 0 is skipped).
		int measured = 0, bad = 0;
		double worst = 0.0;
		std::string firstWhy;
		std::map< int64_t, std::vector< double > > byField;//weights in aperture order
		for( const WeightReading& r : readings )
		{
			const Frac p      = positionOf( r.j, d );
			const Taps stated = statedTemporal( cs.temporal, p );
			if( stated.first < 0 )
				continue;
			double w = 0.0;
			std::string why;
			if( !measuredAperture( r, stated.count, stated, w, why ) )
			{
				if( bad++ == 0 )
					firstWhy = "field " + std::to_string( r.j ) + ": " + why;
				continue;
			}
			++measured;
			worst = std::max( worst, w );
			std::vector< double > ordered;
			for( int k = 0; k < stated.count; ++k )
				ordered.push_back( r.onehot[ ( ( stated.first + k ) % 4 + 4 ) % 4 ] );
			byField[ r.j ] = ordered;
		}

		const bool ok = bad == 0 && measured >= 6 * ( cycles - 2 );
		const char* tname = cs.temporal == model::kLinear ? "Linear" : "Four Field";
		failures += report( ok, quiet, "weights %-18s %-10s %d fields measured against the time geometry, worst %.2e%s%s", d.name,
		                    tname, measured, worst, bad ? "; first mismatch: " : "", firstWhy.c_str() );

		//50 -> 60: the weights, by field in the aperture, repeat exactly
		//every six destination fields -- bit for bit, because p_j's fraction
		//is the same rational and the same float.
		if( d.dst.rate.den == 1 && d.dst.rate.num == 60 && d.src.rate.num == 50 )
		{
			int compared = 0, differ = 0;
			for( const auto& e : byField )
			{
				auto next = byField.find( e.first + 6 );
				if( next == byField.end() )
					continue;
				++compared;
				if( next->second != e.second )
					++differ;
			}
			failures += report( differ == 0 && compared >= 6 * ( cycles - 3 ), quiet,
			                    "weights %-18s %-10s six-field cycle: %d pairs j, j+6 compared, %d differ in any bit", d.name, tname,
			                    compared, differ );
		}

		//50 -> 59.94 under Linear: the weight on the later field is p_j's
		//fraction, measured; six fields on it has moved by 6 x 1001/1200 - 5
		//= 0.005 of a source field. Checked on every pair that does not wrap.
		if( d.dst.rate.den == 1001 && cs.temporal == model::kLinear )
		{
			const double creep = 6.0 * 1001.0 / 1200.0 - 5.0;
			int pairs = 0, off = 0;
			double sum = 0.0, worstErr = 0.0;
			for( const auto& e : byField )
			{
				auto next = byField.find( e.first + 6 );
				if( next == byField.end() )
					continue;
				const double a = e.second[ 1 ], b = next->second[ 1 ];
				if( a < 0.01 || b > 0.99 || b < a - 0.5 )
					continue;//wrapped past a field boundary
				const double tol = halfUlp( a ) + halfUlp( b ) + 2e-7;
				const double err = std::fabs( ( b - a ) - creep );
				worstErr         = std::max( worstErr, err );
				if( err > tol )
					++off;
				sum += b - a;
				++pairs;
			}
			const double mean = pairs ? sum / pairs : 0.0;
			failures += report( pairs >= 60 && off == 0, quiet,
			                    "weights %-18s %-10s creep per six-field cycle %.6f over %d pairs, stated %.6f, worst error %.1e", d.name,
			                    tname, mean, pairs, creep, worstErr );
		}
	}
	if( !quiet )
		std::printf( "weights: %s\n", failures == 0 ? "every destination field's weights are where the time geometry puts them"
		                                             : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --judder
//
// A vertical bar Wb pixels wide moving v pixels per source field, rendered
// on the CPU with exact coverage at its true position at every host frame.
// Each destination field is read out of the Bob display (horizontally the
// store is the host's own columns), and the bar's components found as runs
// of non-zero pixels: their mass is the weight, their centroid the position.
// A box of integer width sampled by pixel coverage has its pixel centroid
// EXACTLY at its centre, so a fractional position is recoverable too; what
// differs is the store's rounding of a fractional coverage, and that is the
// fractional tolerance.
//---------------------------------------------------------------------------
std::vector< float > barFrame( int width, int height, double left, double barWidth, double background = 0.0 )
{
	std::vector< float > img( static_cast< size_t >( width ) * height * 4, 0.0f );
	std::vector< float > row( static_cast< size_t >( width ), 0.0f );
	for( int x = 0; x < width; ++x )
	{
		const double cover = std::max( 0.0, std::min( x + 1.0, left + barWidth ) - std::max( static_cast< double >( x ), left ) );
		row[ x ]           = static_cast< float >( background + ( 1.0 - background ) * cover );
	}
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			float* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = row[ x ];
			px[ 3 ]                     = 1.0f;
		}
	return img;
}

struct Component
{
	double mass;
	double centroid;
	double tolerance;///< on the centroid, from the store's rounding of each pixel
};

std::vector< Component > componentsOf( const std::vector< float >& row, int width )
{
	std::vector< Component > found;
	int x = 0;
	while( x < width )
	{
		if( row[ static_cast< size_t >( x ) * 4 ] <= 0.0f )
		{
			++x;
			continue;
		}
		double mass = 0.0, moment = 0.0;
		int start = x;
		for( ; x < width && row[ static_cast< size_t >( x ) * 4 ] > 0.0f; ++x )
		{
			const double v = row[ static_cast< size_t >( x ) * 4 ];
			mass += v;
			moment += ( x + 0.5 ) * v;
		}
		Component c;
		c.mass     = mass;
		c.centroid = moment / mass;
		double tol = 0.0;
		for( int i = start; i < x; ++i )
		{
			const double v = row[ static_cast< size_t >( i ) * 4 ];
			tol += std::fabs( i + 0.5 - c.centroid ) * halfUlp( v );
		}
		c.tolerance = tol / mass + 1e-9;
		found.push_back( c );
	}
	return found;
}

int runJudder( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const double barWidth = 3.0;
	const double x0       = 8.0;

	struct Case
	{
		const char* kind;
		int temporal;
		double v;
	};
	const Case cases[] = {
		{ "Drop/Repeat, whole pixels", model::kDropRepeat, 4.0 },
		{ "Drop/Repeat, fractional  ", model::kDropRepeat, 2.3 },
		{ "Linear, whole pixels     ", model::kLinear, 8.0 },
		{ "Linear, fractional       ", model::kLinear, 7.3 },
	};

	const Dir& d = kDir625To525At60;
	for( const Case& cs : cases )
	{
		Session s;
		baseline( s.plugin, d, cs.temporal );
		s.plugin.SetPerturbForTest( perturb );
		int64_t m = 0;
		placeFieldsExactly( s, d, m );
		if( !s.begin( width, height ) )
			return failures + 1;

		//Where the bar's left edge is at time t: v px per source field.
		auto leftAt = [ & ]( double fields ) { return x0 + cs.v * fields; };

		int measured = 0, bad = 0, repeats = 0, statedRepeats = 0;
		double worstWhole = 0.0, worstFrac = 0.0;
		std::string firstWhy;
		int64_t lastShown   = INT64_MIN;
		double lastCentroid = 0.0;
		int64_t lastField   = INT64_MIN;
		int64_t lastSource  = INT64_MIN;
		const bool fractional = std::fabs( cs.v - std::floor( cs.v ) ) > 0.0;

		for( int64_t k = 0; measured + bad < 24 && k < 20000; ++k )
		{
			//The bar at this frame's own instant, in source fields: k / M.
			const double fieldsNow         = static_cast< double >( k ) / static_cast< double >( m );
			const std::vector< float > img = barFrame( s.width, s.height, leftAt( fieldsNow ), barWidth );
			if( !s.render( k, img ) )
				break;
			int64_t older = 0, newer = 0;
			s.plugin.LastDisplayForTest( older, newer );
			if( newer == lastShown )
				continue;
			lastShown = newer;

			const Frac p      = positionOf( newer, d );
			const Taps stated = statedTemporal( cs.temporal, p );
			if( stated.first < 1 )
				continue;//an aperture reaching into the primed store

			const std::vector< float > row       = s.readRow( s.height / 2 );
			const std::vector< Component > comps = componentsOf( row, s.width );

			//The stated picture: one component per field with a non-zero
			//weight, at that field's bar centre, with mass Wb x weight.
			std::vector< std::pair< double, double > > want;//centre, weight
			for( int kk = 0; kk < stated.count; ++kk )
				if( stated.w[ kk ] != 0.0 )
					want.emplace_back( leftAt( static_cast< double >( stated.first + kk ) ) + barWidth / 2.0, stated.w[ kk ] );

			std::string why;
			bool ok = comps.size() == want.size();
			if( !ok )
				why = std::to_string( comps.size() ) + " components, stated " + std::to_string( want.size() );
			for( size_t i = 0; ok && i < want.size(); ++i )
			{
				const double posErr = std::fabs( comps[ i ].centroid - want[ i ].first );
				const double wErr   = std::fabs( comps[ i ].mass / barWidth - want[ i ].second );
				//Weight: each pixel rounded by at most a half-ULP of itself,
				//Wb of them, over Wb.
				const double wTol   = halfUlp( want[ i ].second ) + 1e-7;
				//Position: exact for whole pixels (every value is 0, 1 or the
				//weight, and the centroid of a box's coverage is its centre);
				//fractional, the store's rounding of each edge's coverage.
				const double posTol = fractional ? comps[ i ].tolerance : 1e-9;
				double& worst       = fractional ? worstFrac : worstWhole;
				worst               = std::max( worst, posErr );
				if( posErr > posTol || wErr > wTol )
				{
					ok = false;
					char b[ 200 ];
					std::snprintf( b, sizeof( b ), "component %zu at %.5f weight %.5f, stated %.5f weight %.5f", i,
					               comps[ i ].centroid, comps[ i ].mass / barWidth, want[ i ].first, want[ i ].second );
					why = b;
				}
			}

			//Drop/Repeat: a repeat is a field whose bar did not move. Counted
			//out of the picture and out of the geometry, separately.
			if( cs.temporal == model::kDropRepeat && ok && comps.size() == 1 )
			{
				if( lastField == newer - 1 )
				{
					if( std::fabs( comps[ 0 ].centroid - lastCentroid ) < 1e-9 )
						++repeats;
					if( stated.first == lastSource )
						++statedRepeats;
				}
				lastCentroid = comps[ 0 ].centroid;
				lastField    = newer;
				lastSource   = stated.first;
			}

			if( ok )
				++measured;
			else if( bad++ == 0 )
				firstWhy = "field " + std::to_string( newer ) + ": " + why;
		}

		s.end();
		char extra[ 128 ] = "";
		bool ok           = bad == 0 && measured >= 20;
		if( cs.temporal == model::kDropRepeat )
		{
			//50 -> 60: one repeat in every six fields, from the geometry; the
			//picture has to show exactly the repeats the geometry states.
			std::snprintf( extra, sizeof( extra ), "; %d repeats seen, %d stated (one in six)", repeats, statedRepeats );
			ok = ok && repeats == statedRepeats && statedRepeats >= 3;
		}
		failures += report( ok, quiet, "judder %s v=%.1f px/field: %d fields at the stated positions and weights, worst %.1e px%s%s%s",
		                    cs.kind, cs.v, measured, fractional ? worstFrac : worstWhole, extra, bad ? "; first mismatch: " : "",
		                    firstWhy.c_str() );
	}
	if( !quiet )
		std::printf( "judder: %s\n", failures == 0 ? "the repeat cycle and the double image are where the time geometry puts them" : "FAILURES" );
	return failures;
}


//---------------------------------------------------------------------------
// --lines
//
// Part 1, at --size: a still picture with marked host rows, one tap, the
// nearest field, woven onto the host's raster and at native lines. Every
// output row's brightness is predicted by composing three integer maps --
// the source line that samples a host row, the nearest source field line to
// a destination line in the field being read, the destination line an output
// row shows -- and must match exactly. All four (destination parity, source
// parity) pairs have to be seen.
//
// Part 2, raster-free vertically: the host picture is the source standard's
// own line count high (--size's width), so that each host row IS one source
// line and a field's lines are exactly what the harness put there. Through
// each tap count:
//   - impulses spaced beyond the filter's support read back every tap at
//     every destination line's phase: the impulse response;
//   - a vertical zone plate (frequency across the picture, 0 to the field's
//     Nyquist) is fitted, per phase class of destination lines, for the
//     complex gain G( f ) the destination lines carry: the measured vertical
//     frequency response, against the stated taps' transfer function.
//---------------------------------------------------------------------------
int hostRowOfLine( int l, int hostHeight, int lines )
{
	return static_cast< int >( ( ( 2 * static_cast< int64_t >( l ) + 1 ) * hostHeight ) / ( 2 * static_cast< int64_t >( lines ) ) );
}

int runLineMap( int width, int height, int perturb, bool quiet )
{
	int failures = 0;
	for( const Dir* dp : { &kDir625To525, &kDir525To625, &kDirSame } )
	{
		const Dir& d = *dp;
		const int ls = d.src.lines, ld = d.dst.lines;

		//Marked host rows: the rows three chosen source lines sample, so a
		//mark always exists in the source even where the host has more rows
		//than the standard has lines.
		std::set< int > marked;
		for( int l0 : { ls / 5, ls / 2 + 1, ( 4 * ls ) / 5 + 2 } )
			marked.insert( hostRowOfLine( l0, height, ls ) );

		std::vector< float > img( static_cast< size_t >( width ) * height * 4, 0.0f );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				float* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
				px[ 0 ] = px[ 1 ] = px[ 2 ] = marked.count( y ) ? 1.0f : 0.0f;
				px[ 3 ]                     = 1.0f;
			}

		for( int outputSize : { 1, 0 } )
		{
			Session s;
			baseline( s.plugin, d, model::kDropRepeat );
			set( s.plugin, "Show As", 0.0f );
			set( s.plugin, "Output Size", static_cast< float >( outputSize ) );
			s.plugin.SetPerturbForTest( perturb );
			if( !s.begin( width, height ) )
				return failures + 1;

			int frames = 0, badRows = 0, litRows = 0;
			std::set< int > combos;
			std::string firstWhy;
			for( int64_t k = 0; k < 48; ++k )
			{
				if( !s.render( k, img ) )
					break;
				if( k < 8 )
					continue;
				int64_t older = 0, newer = 0;
				s.plugin.LastDisplayForTest( older, newer );
				const std::vector< float > out = s.readBackFloat();
				++frames;

				for( int Y = 0; Y < height; ++Y )
				{
					const int m = outputSize == 1 ? static_cast< int >( ( ( 2 * static_cast< int64_t >( Y ) + 1 ) * ld ) / ( 2 * static_cast< int64_t >( height ) ) )
					                              : Y - static_cast< int >( floorDiv( height - ld, 2 ) );
					bool lit = false;
					if( m >= 0 && m < ld )
					{
						const int q       = m & 1;
						const int64_t j   = q == parityOf( newer ) ? newer : older;
						const int64_t n   = statedTemporal( model::kDropRepeat, positionOf( j, d ) ).first;
						const int p       = parityOf( n );
						combos.insert( q * 2 + p );
						const Frac u      = sourceLineOf( m, p, d );
						const int64_t fk  = std::clamp< int64_t >( statedTaps( 1, u ).first, 0, ls / 2 - 1 );
						const int l       = static_cast< int >( 2 * fk + p );
						lit               = marked.count( hostRowOfLine( l, height, ls ) ) > 0;
					}
					litRows += lit ? 1 : 0;
					for( int x = 0; x < width; x += std::max( 1, width / 8 ) )
					{
						const bool seen = out[ ( static_cast< size_t >( Y ) * width + x ) * 4 ] > 0.5f;
						if( seen != lit )
						{
							if( badRows++ == 0 )
							{
								char b[ 160 ];
								std::snprintf( b, sizeof( b ), "frame %lld row %d (line %d) is %s, the geometry says %s",
								               static_cast< long long >( k ), Y, m, seen ? "lit" : "dark", lit ? "lit" : "dark" );
								firstWhy = b;
							}
							break;
						}
					}
				}
			}
			s.end();
			//A conversion reads both source parities into both destination
			//parities; Same reads field j into field j, so only q = p exists.
			const size_t wantCombos = &d == &kDirSame ? 2 : 4;
			const bool ok           = badRows == 0 && combos.size() == wantCombos && litRows > 0;
			failures += report( ok, quiet, "lines %-18s %-6s marked host rows land where the geometry says: %d frames, %d rows lit, %zu of %zu parity pairs%s%s",
			                    d.name, outputSize == 1 ? "host" : "native", frames, litRows, combos.size(), wantCombos,
			                    badRows ? "; " : "", firstWhy.c_str() );
		}
	}
	return failures;
}

/// Destination fields, with the source parity each was read from, until all
/// four (q, p) pairs are in hand. The picture is still, so which field of a
/// parity is read does not matter; which parity does.
bool collectFields( int width, const Dir& d, int taps, double softness, int perturb, const std::vector< float >& img,
                    std::map< int, std::vector< float > >& byCombo, int& fieldWidth )
{
	Session s;
	baseline( s.plugin, d, model::kDropRepeat );
	set( s.plugin, "Vertical Taps", static_cast< float >( taps == 1 ? 0 : taps == 2 ? 1 : taps == 4 ? 2 : 3 ) );
	set( s.plugin, "Softness", static_cast< float >( softness ) );
	s.plugin.SetPerturbForTest( perturb );
	if( !s.begin( width, d.src.lines ) )
		return false;
	byCombo.clear();
	int64_t lastShown = INT64_MIN;
	for( int64_t k = 0; k < 400 && byCombo.size() < 4; ++k )
	{
		if( !s.render( k, img ) )
			break;
		if( k < 8 )
			continue;
		int64_t older = 0, newer = 0;
		s.plugin.LastDisplayForTest( older, newer );
		if( newer == lastShown )
			continue;
		lastShown       = newer;
		const int q     = parityOf( newer );
		const int p     = parityOf( statedTemporal( model::kDropRepeat, positionOf( newer, d ) ).first );
		const int combo = q * 2 + p;
		if( byCombo.count( combo ) )
			continue;
		std::vector< float > field;
		int w = 0, lines = 0;
		if( s.plugin.ReadDestinationFieldForTest( newer, field, w, lines ) && lines == d.dst.lines / 2 )
		{
			byCombo[ combo ] = field;
			fieldWidth       = w;
		}
	}
	s.end();
	return byCombo.size() == 4;
}

/// The stated composite filter: Softness's [ a, 1 - 2a, a ] in front of the
/// stated interpolator, as taps on field lines.
std::vector< std::pair< int64_t, double > > statedComposite( int taps, double softness, const Frac& u )
{
	const Taps t = statedTaps( taps, u );
	std::map< int64_t, double > w;
	const double a = softness / 4.0;
	for( int i = 0; i < t.count; ++i )
	{
		if( softness <= 0.0 )
			w[ t.first + i ] += t.w[ i ];
		else
		{
			w[ t.first + i - 1 ] += a * t.w[ i ];
			w[ t.first + i ] += ( 1.0 - 2.0 * a ) * t.w[ i ];
			w[ t.first + i + 1 ] += a * t.w[ i ];
		}
	}
	return { w.begin(), w.end() };
}

int runImpulse( int width, int perturb, bool quiet )
{
	int failures = 0;
	struct Case
	{
		int taps;
		double softness;
	};
	const Case cases[] = { { 1, 0.0 }, { 2, 0.0 }, { 4, 0.0 }, { 8, 0.0 }, { 4, 0.8 } };
	for( const Dir* dp : { &kDir625To525, &kDir525To625 } )
	{
		const Dir& d  = *dp;
		const int ls  = d.src.lines;
		const int lfs = ls / 2, lfd = d.dst.lines / 2;

		//Impulses on frame lines 20i + ( i & 1 ): both parities, 20 field
		//lines apart in each field, beyond any filter's reach (ten lines).
		std::vector< int > lit( static_cast< size_t >( ls ), 0 );
		for( int i = 1; 20 * i + 1 < ls - 4; ++i )
			lit[ static_cast< size_t >( 20 * i + ( i & 1 ) ) ] = 1;
		std::vector< float > img( static_cast< size_t >( width ) * ls * 4, 0.0f );
		for( int y = 0; y < ls; ++y )
			for( int x = 0; x < width; ++x )
			{
				float* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
				px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( lit[ static_cast< size_t >( y ) ] );
				px[ 3 ]                     = 1.0f;
			}

		for( const Case& cs : cases )
		{
			std::map< int, std::vector< float > > fields;
			int fw = 0;
			if( !collectFields( width, d, cs.taps, cs.softness, perturb, img, fields, fw ) )
			{
				failures += report( false, quiet, "impulse %s %d taps: could not see all four parity pairs", d.name, cs.taps );
				continue;
			}
			int checked = 0, bad = 0;
			double worst = 0.0;
			std::string firstWhy;
			for( const auto& e : fields )
			{
				const int q = e.first / 2, p = e.first % 2;
				for( int i = 0; i < lfd; ++i )
				{
					const int m  = 2 * i + q;
					const Frac u = sourceLineOf( m, p, d );
					double want  = 0.0;
					for( const auto& t : statedComposite( cs.taps, cs.softness, u ) )
					{
						const int64_t k = std::clamp< int64_t >( t.first, 0, lfs - 1 );
						want += t.second * lit[ static_cast< size_t >( 2 * k + p ) ];
					}
					const double got = e.second[ ( static_cast< size_t >( i ) * fw + fw / 2 ) * 4 ];
					const double err = std::fabs( got - want );
					//The store's rounding of the one tap that sees an impulse,
					//and that tap's rounding into float.
					const double tol = halfUlp( want ) + 2e-7;
					worst            = std::max( worst, err );
					++checked;
					if( err > tol && bad++ == 0 )
					{
						char b[ 200 ];
						std::snprintf( b, sizeof( b ), "q=%d p=%d line %d reads %.6f, stated %.6f", q, p, i, got, want );
						firstWhy = b;
					}
				}
			}
			failures += report( bad == 0 && checked > 0, quiet,
			                    "lines %-18s impulse response, %d taps%s: %d destination lines in four parity pairs, worst %.1e%s%s", d.name,
			                    cs.taps, cs.softness > 0 ? " + softness 0.8" : "", checked, worst, bad ? "; " : "", firstWhy.c_str() );
		}
	}
	return failures;
}

int runZonePlate( int width, int perturb, bool quiet )
{
	int failures = 0;
	for( const Dir* dp : { &kDir625To525, &kDir525To625 } )
	{
		const Dir& d  = *dp;
		const int ls  = d.src.lines;
		const int lfd = d.dst.lines / 2;

		//Frame line l, column x: 0.5 + 0.4 cos( 2 pi f l / 2 ), with f from 0
		//to 0.5 cycles per FIELD line across the picture. Field p's line k is
		//frame line 2k + p, so each field holds a pure cosine of frequency f
		//in its own lines, sampled at k + p / 2.
		auto freqOf = [ & ]( int x ) { return 0.5 * ( x + 0.5 ) / width; };
		std::vector< float > img( static_cast< size_t >( width ) * ls * 4, 0.0f );
		for( int y = 0; y < ls; ++y )
			for( int x = 0; x < width; ++x )
			{
				float* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
				px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( 0.5 + 0.4 * std::cos( M_PI * freqOf( x ) * y ) );
				px[ 3 ]                     = 1.0f;
			}

		for( int taps : { 2, 4, 8 } )
		{
			std::map< int, std::vector< float > > fields;
			int fw = 0;
			if( !collectFields( width, d, taps, 0.0, perturb, img, fields, fw ) )
			{
				failures += report( false, quiet, "zone plate %s %d taps: could not see all four parity pairs", d.name, taps );
				continue;
			}

			int fits = 0, bad = 0;
			double worstRatio = 0.0, gainAtQuarter = -1.0;
			std::string firstWhy;
			for( const auto& e : fields )
			{
				const int q = e.first / 2, p = e.first % 2;
				//Phase classes: destination lines whose position has the same
				//fraction share one set of taps, so one G.
				std::map< int64_t, std::vector< int > > classes;
				for( int i = 6; i < lfd - 6; ++i )
					classes[ sourceLineOf( 2 * i + q, p, d ).rem ].push_back( i );

				for( int x = 0; x < fw; x += std::max( 1, fw / 24 ) )
				{
					const double f = freqOf( x );
					if( f < 0.03 )
						continue;
					for( const auto& cl : classes )
					{
						//Least squares for ( a, b ) in o - 1/2 = 0.4 ( a cos th - b sin th ),
						//th = 2 pi f ( u + p / 2 ).
						double sxx = 0, sxy = 0, syy = 0, sxo = 0, syo = 0;
						std::vector< std::pair< double, double > > basis;
						const Frac u0 = sourceLineOf( 2 * cl.second[ 0 ] + q, p, d );
						std::complex< double > stated = 0.0;
						double absSum                 = 0.0;
						for( const auto& t : statedComposite( taps, 0.0, u0 ) )
						{
							const double pos = static_cast< double >( u0.n ) + u0.phi();
							stated += t.second * std::polar( 1.0, 2.0 * M_PI * f * ( static_cast< double >( t.first ) - pos ) );
							absSum += std::fabs( t.second );
						}
						for( int i : cl.second )
						{
							const Frac u   = sourceLineOf( 2 * i + q, p, d );
							const double th = 2.0 * M_PI * f * ( static_cast< double >( u.n ) + u.phi() + 0.5 * p );
							const double cx = 0.4 * std::cos( th ), cy = -0.4 * std::sin( th );
							const double o  = e.second[ ( static_cast< size_t >( i ) * fw + x ) * 4 ] - 0.5;
							sxx += cx * cx;
							sxy += cx * cy;
							syy += cy * cy;
							sxo += cx * o;
							syo += cy * o;
							basis.emplace_back( cx, cy );
						}
						const double det = sxx * syy - sxy * sxy;
						if( std::fabs( det ) < 1e-12 )
							continue;
						const double a = ( syy * sxo - sxy * syo ) / det;
						const double b = ( sxx * syo - sxy * sxo ) / det;

						//The tolerance, from the design itself: the fit is
						//M e with M = ( X'X )^-1 X', so |delta a|, |delta b| <=
						//max row sum of |M| times the largest per-sample error --
						//the input's rounding into the half store carried
						//through the taps, plus the output's own rounding.
						double rowA = 0.0, rowB = 0.0;
						for( const auto& bs : basis )
						{
							rowA += std::fabs( ( syy * bs.first - sxy * bs.second ) / det );
							rowB += std::fabs( ( sxx * bs.second - sxy * bs.first ) / det );
						}
						const double eMax = absSum * halfUlp( 0.9 ) + halfUlp( 1.0 ) + 1e-6;
						const double tol  = std::hypot( rowA, rowB ) * eMax;
						if( tol > 0.05 )
							continue;//this class samples f too sparsely to say anything
						const std::complex< double > measured( a, b );
						const double err = std::abs( measured - stated );
						++fits;
						worstRatio = std::max( worstRatio, err / tol );
						if( std::fabs( f - 0.25 ) < 0.02 && gainAtQuarter < 0.0 )
							gainAtQuarter = std::abs( measured );
						if( err > tol && bad++ == 0 )
						{
							char buf[ 220 ];
							std::snprintf( buf, sizeof( buf ), "q=%d p=%d f=%.3f class %lld: G %.5f%+.5fi, stated %.5f%+.5fi, tolerance %.1e", q,
							               p, f, static_cast< long long >( cl.first ), a, b, stated.real(), stated.imag(), tol );
							firstWhy = buf;
						}
					}
				}
			}
			failures += report( bad == 0 && fits >= 40, quiet,
			                    "lines %-18s zone plate, %d taps: %d fits of G(f) per phase class, worst %.2f of its tolerance, |G(0.25)| %.4f%s%s",
			                    d.name, taps, fits, worstRatio, gainAtQuarter, bad ? "; " : "", firstWhy.c_str() );
		}
	}
	return failures;
}

int runLines( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = runLineMap( width, height, perturb, quiet );
	failures += runImpulse( width, perturb, quiet );
	failures += runZonePlate( width, perturb, quiet );
	if( !quiet )
		std::printf( "lines: %s\n", failures == 0 ? "marked lines land where the geometry says, and the taps are the stated filter" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --same
//
// Same direction, Drop/Repeat, one tap, woven onto the host's raster at
// 60 frames a second -- a rate that does NOT put fields on host frames, so
// the nearest-frame capture is exercised too. Every host frame is a
// different 8-bit picture. The output is predicted pixel for pixel from the
// geometry alone: output row Y shows frame line m, from the destination field
// of m's parity in the pair on show, which is source field j, captured from
// the host frame nearest j / 50, row floor( ( 2m + 1 ) H / 1152 ). Compared
// byte for byte. And the store: for Same every tap count is the identity,
// so each destination field must equal its source field in every bit.
//---------------------------------------------------------------------------
std::vector< unsigned char > hashFrame( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			uint32_t h = static_cast< uint32_t >( frame * 73856093 ) ^ static_cast< uint32_t >( y * 19349663 ) ^ static_cast< uint32_t >( x * 83492791 );
			h          = h * 747796405u + 2891336453u;
			h          = ( ( h >> ( ( h >> 28u ) + 4u ) ) ^ h ) * 277803737u;
			h          = ( h >> 22u ) ^ h;
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = static_cast< unsigned char >( h );
			px[ 1 ] = static_cast< unsigned char >( h >> 8 );
			px[ 2 ] = static_cast< unsigned char >( h >> 16 );
			px[ 3 ] = 255;
		}
	return img;
}

int runSame( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const Dir& d = kDirSame;
	const int ls = d.src.lines;

	{
		Session s;
		s.floatOutput = false;
		baseline( s.plugin, d, model::kDropRepeat );
		set( s.plugin, "Show As", 0.0f );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return 1;

		std::vector< std::vector< unsigned char > > frames;
		int compared = 0, differ = 0, pairWrong = 0;
		std::string firstWhy;
		for( int64_t k = 0; k < 90; ++k )
		{
			frames.push_back( hashFrame( width, height, k ) );
			if( !s.render( k, frames.back() ) )
				break;
			if( k < 12 )
				continue;

			//The pair on show, from the time geometry: field j is released at
			//j / 50 + 2 / 50, so the latest is floor( 50 t ) - 2, and Weave
			//shows the latest complete pair ( 2i, 2i + 1 ).
			const double t     = s.timeOf( k );
			const int64_t J    = static_cast< int64_t >( std::floor( t * 50.0 + 1e-9 ) ) - 2;
			const int64_t newer = parityOf( J ) == 1 ? J : J - 1;
			const int64_t older = newer - 1;
			int64_t po = 0, pn = 0;
			s.plugin.LastDisplayForTest( po, pn );
			if( po != older || pn != newer )
				++pairWrong;

			const std::vector< unsigned char > out = s.readBack();
			bool frameOk                           = true;
			for( int Y = 0; Y < height && frameOk; ++Y )
			{
				const int m        = static_cast< int >( ( ( 2 * static_cast< int64_t >( Y ) + 1 ) * ls ) / ( 2 * static_cast< int64_t >( height ) ) );
				const int64_t j    = parityOf( m ) == parityOf( newer ) ? newer : older;
				//Nearest host frame to j / 50 at 60 frames a second: round( 1.2 j ),
				//never a tie. Fields at or before zero were primed from frame 0.
				const int64_t kk   = j <= 0 ? 0 : static_cast< int64_t >( std::floor( 1.2 * static_cast< double >( j ) + 0.5 ) );
				const int r        = hostRowOfLine( m, height, ls );
				const unsigned char* want = frames[ static_cast< size_t >( kk ) ].data() + static_cast< size_t >( r ) * width * 4;
				const unsigned char* got  = out.data() + static_cast< size_t >( Y ) * width * 4;
				if( std::memcmp( want, got, static_cast< size_t >( width ) * 4 ) != 0 )
				{
					frameOk = false;
					if( differ == 0 )
					{
						char b[ 160 ];
						std::snprintf( b, sizeof( b ), "frame %lld row %d (line %d, field %lld, host frame %lld)", static_cast< long long >( k ), Y,
						               m, static_cast< long long >( j ), static_cast< long long >( kk ) );
						firstWhy = b;
					}
				}
			}
			++compared;
			if( !frameOk )
				++differ;
		}
		s.end();
		failures += report( differ == 0 && pairWrong == 0 && compared > 60, quiet,
		                    "same: %d frames woven from the nearest host frames, %d differ in any byte, %d showed the wrong pair%s%s", compared,
		                    differ, pairWrong, differ ? "; first: " : "", firstWhy.c_str() );
	}

	//The store: every destination field equals its source field bit for bit,
	//at every tap count -- each interpolator is the identity on a line.
	for( int tapOption = 0; tapOption < 4; ++tapOption )
	{
		Session s;
		baseline( s.plugin, d, model::kDropRepeat );
		set( s.plugin, "Vertical Taps", static_cast< float >( tapOption ) );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return failures + 1;
		int compared = 0, differ = 0;
		for( int64_t k = 0; k < 40; ++k )
		{
			if( !s.render( k, hashFrame( width, height, k ) ) )
				break;
			int64_t older = 0, newer = 0;
			s.plugin.LastDisplayForTest( older, newer );
			if( k < 12 )
				continue;
			std::vector< float > a, b;
			int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
			if( !s.plugin.ReadDestinationFieldForTest( newer, a, w1, h1 ) || !s.plugin.ReadSourceFieldForTest( newer, b, w2, h2 ) )
				continue;
			++compared;
			if( a.size() != b.size() || std::memcmp( a.data(), b.data(), a.size() * sizeof( float ) ) != 0 )
				++differ;
		}
		s.end();
		failures += report( differ == 0 && compared > 10, quiet, "same: %d taps, %d destination fields against their source fields, %d differ in any bit",
		                    model::kTapCounts[ tapOption ], compared, differ );
	}
	if( !quiet )
		std::printf( "same: %s\n", failures == 0 ? "Same returns the fielded input, bit for bit" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --mc
//
// A pure translation: a bar moving 3 px per source field, 50 -> 60 with
// Motion Comp. Every destination field must put the bar at its own instant,
// x0 + v p_j, so the displacement per field is uniform at v 50/60 = 2.5 px.
// The spec's tolerance, one pixel, is also the method's: the block vector is
// an integer. Without Motion Comp the same material judders by more than a
// pixel, which is what there was to remove.
//
// An occlusion: a 24 px bar moving 6 px per field over a textured background
// standing still. Blocks that hold both motions get one vector and tear.
// Outside the blocks within reach of the bar's edges the output must be the
// ideal picture to the store's rounding; inside, a tear of at least 0.25
// must appear somewhere in the run. Where it fails is asserted, not hoped.
//---------------------------------------------------------------------------
double backgroundAt( int x )
{
	const double c = x + 0.5;
	return 0.5 + 0.2 * std::sin( 2.0 * M_PI * c / 23.0 ) + 0.15 * std::sin( 2.0 * M_PI * c / 9.7 );
}

std::vector< float > occlusionFrame( int width, int height, double left, double barWidth )
{
	std::vector< float > img( static_cast< size_t >( width ) * height * 4, 0.0f );
	std::vector< float > row( static_cast< size_t >( width ) );
	for( int x = 0; x < width; ++x )
	{
		const double cover = std::max( 0.0, std::min( x + 1.0, left + barWidth ) - std::max( static_cast< double >( x ), left ) );
		row[ x ]           = static_cast< float >( cover + ( 1.0 - cover ) * backgroundAt( x ) );
	}
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			float* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = row[ x ];
			px[ 3 ]                     = 1.0f;
		}
	return img;
}

int runMc( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const Dir& d = kDir625To525At60;
	const double stepPerField = 50.0 / 60.0;

	//--- translation -------------------------------------------------------
	struct Case
	{
		int temporal;
		bool motion;
	};
	const Case cases[] = { { model::kDropRepeat, true }, { model::kLinear, true }, { model::kDropRepeat, false } };
	for( const Case& cs : cases )
	{
		const double v = 3.0, barWidth = 3.0, x0 = 40.0;
		Session s;
		baseline( s.plugin, d, cs.temporal );
		set( s.plugin, "Motion Comp", cs.motion ? 1.0f : 0.0f );
		s.plugin.SetPerturbForTest( perturb );
		int64_t m = 0;
		placeFieldsExactly( s, d, m );
		if( !s.begin( width, height ) )
			return failures + 1;

		int measured = 0, bad = 0, singles = 0;
		double worstPos = 0.0, worstStep = 0.0;
		int64_t lastShown = INT64_MIN, lastField = INT64_MIN;
		double lastCentroid = 0.0;
		for( int64_t k = 0; measured < 24 && k < 20000; ++k )
		{
			const double fieldsNow = static_cast< double >( k ) / static_cast< double >( m );
			if( !s.render( k, barFrame( s.width, s.height, x0 + v * fieldsNow, barWidth ) ) )
				break;
			int64_t older = 0, newer = 0;
			s.plugin.LastDisplayForTest( older, newer );
			if( newer == lastShown )
				continue;
			lastShown    = newer;
			const Frac p = positionOf( newer, d );
			if( p.n < 2 )
				continue;
			const std::vector< Component > comps = componentsOf( s.readRow( s.height / 2 ), s.width );
			double mass = 0.0, moment = 0.0;
			for( const Component& c : comps )
			{
				mass += c.mass;
				moment += c.mass * c.centroid;
			}
			if( mass <= 0.0 )
			{
				++bad;
				continue;
			}
			const double centroid = moment / mass;
			const double want     = x0 + v * ( static_cast< double >( p.n ) + p.phi() ) + barWidth / 2.0;
			worstPos              = std::max( worstPos, std::fabs( centroid - want ) );
			if( comps.size() == 1 )
				++singles;
			if( lastField == newer - 1 )
				worstStep = std::max( worstStep, std::fabs( ( centroid - lastCentroid ) - v * stepPerField ) );
			lastField    = newer;
			lastCentroid = moment / mass;
			++measured;
		}
		s.end();

		const char* tname = cs.temporal == model::kLinear ? "Linear     " : "Drop/Repeat";
		if( cs.motion )
		{
			//Linear with MC: the two fields' bars land on each other, so the
			//double image is one image.
			const bool ok = bad == 0 && measured >= 20 && worstStep <= 1.0 && worstPos <= 1.0
			                && ( cs.temporal != model::kLinear || singles == measured );
			failures += report( ok, quiet,
			                    "mc %s pan 3 px/field: %d fields, per-field step within %.3f px of %.3f (spec: 1 px), position within %.3f px, %d of %d single images",
			                    tname, measured, worstStep, v * stepPerField, worstPos, singles, measured );
		}
		else
			failures += report( measured >= 20 && worstStep > 1.0, quiet,
			                    "mc %s pan without Motion Comp: per-field step strays %.3f px from %.3f -- the judder there is to remove", tname,
			                    worstStep, v * stepPerField );
	}

	//--- occlusion ---------------------------------------------------------
	{
		const double v = 6.0, barWidth = 24.0, x0 = 40.0;
		Session s;
		baseline( s.plugin, d, model::kDropRepeat );
		set( s.plugin, "Motion Comp", 1.0f );
		s.plugin.SetPerturbForTest( perturb );
		int64_t m = 0;
		placeFieldsExactly( s, d, m );
		if( !s.begin( width, height ) )
			return failures + 1;

		auto leftAt = [ & ]( double fields ) { return x0 + v * fields; };
		int measured = 0, outsideBad = 0;
		double worstOutside = 0.0, worstInside = 0.0;
		int64_t lastShown = INT64_MIN;
		std::string firstWhy;
		for( int64_t k = 0; measured < 18 && k < 20000; ++k )
		{
			const double fieldsNow = static_cast< double >( k ) / static_cast< double >( m );
			if( !s.render( k, occlusionFrame( s.width, s.height, leftAt( fieldsNow ), barWidth ) ) )
				break;
			int64_t older = 0, newer = 0;
			s.plugin.LastDisplayForTest( older, newer );
			if( newer == lastShown )
				continue;
			lastShown    = newer;
			const Frac p = positionOf( newer, d );
			if( p.n < 2 )
				continue;
			const double pos = static_cast< double >( p.n ) + p.phi();
			const std::vector< float > row = s.readRow( s.height / 2 );
			const std::vector< float > ideal = occlusionFrame( s.width, 1, leftAt( pos ), barWidth );

			//Within reach of an edge: two blocks either side of where the bar
			//is in the bracketing fields, the nearest field and the ideal.
			const int64_t nearest = statedTemporal( model::kDropRepeat, p ).first;
			double lo = 1e30, hi = -1e30;
			for( double f : { static_cast< double >( p.n ), static_cast< double >( p.n + 1 ), static_cast< double >( nearest ), pos } )
			{
				lo = std::min( lo, leftAt( f ) );
				hi = std::max( hi, leftAt( f ) + barWidth );
			}
			lo -= 2 * standards::shaders::kBlockWidth;
			hi += 2 * standards::shaders::kBlockWidth;

			for( int x = 0; x < s.width; ++x )
			{
				const double got  = row[ static_cast< size_t >( x ) * 4 ];
				const double want = ideal[ static_cast< size_t >( x ) * 4 ];
				const double err  = std::fabs( got - want );
				if( x + 1 > lo && x < hi )
					worstInside = std::max( worstInside, err );
				else
				{
					worstOutside = std::max( worstOutside, err );
					if( err > halfUlp( want ) + 1e-6 && outsideBad++ == 0 )
					{
						char b[ 160 ];
						std::snprintf( b, sizeof( b ), "field %lld x=%d: %.4f against %.4f", static_cast< long long >( newer ), x, got, want );
						firstWhy = b;
					}
				}
			}
			++measured;
		}
		s.end();
		failures += report( outsideBad == 0 && worstInside > 0.25 && measured >= 12, quiet,
		                    "mc occlusion: %d fields; outside the predicted blocks worst error %.1e; inside, a tear of %.3f%s%s", measured,
		                    worstOutside, worstInside, outsideBad ? "; " : "", firstWhy.c_str() );
	}
	if( !quiet )
		std::printf( "mc: %s\n", failures == 0 ? "a pan is smooth with Motion Comp, and an occlusion tears where predicted" : "FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//
// The weights run, with the raster doubled mid-run. The fields already in
// the store were captured at the old width; the destination fields after the
// resize are made from them, and must carry exactly the weights the time
// geometry says -- which they cannot if the store was cleared, or re-primed
// from the new frame, on the resize.
//---------------------------------------------------------------------------
int runResize( int width, int height, int perturb = 0, bool quiet = false )
{
	const Dir& d = kDir625To525At60;
	std::vector< WeightReading > readings;
	int64_t firstAfter = INT64_MIN;
	const int64_t resizeAt = 12 * 40 + 5;//host frame, mid-run
	if( !readWeights( width, height, d, model::kLinear, perturb, 80, readings, resizeAt, &firstAfter ) )
		return report( false, quiet, "resize: could not render" );

	int measured = 0, bad = 0, spanning = 0;
	std::string firstWhy;
	const int64_t lastFieldBefore = floorDiv( resizeAt - 1, 12 );
	for( const WeightReading& r : readings )
	{
		const Frac p      = positionOf( r.j, d );
		const Taps stated = statedTemporal( model::kLinear, p );
		if( stated.first < 0 )
			continue;
		double w = 0.0;
		std::string why;
		if( !measuredAperture( r, stated.count, stated, w, why ) )
		{
			if( bad++ == 0 )
				firstWhy = "field " + std::to_string( r.j ) + ": " + why;
			continue;
		}
		++measured;
		//Shown after the resize, made from a field captured before it.
		if( firstAfter != INT64_MIN && r.j >= firstAfter && stated.first <= lastFieldBefore )
			++spanning;
	}
	return report( bad == 0 && spanning >= 1 && measured >= 60, quiet,
	               "resize: %dx%d -> %dx%d mid-run; %d fields measured, %d shown after it from fields captured before it, every weight where the geometry says%s%s",
	               width, height, width * 2, height * 2, measured, spanning, bad ? "; " : "", firstWhy.c_str() );
}

//---------------------------------------------------------------------------
// Offline checks: no GL.
//---------------------------------------------------------------------------
model::Conversion conversionOf( const Dir& d, int perturb )
{
	return model::ConversionOf( d.option, perturb );
}

/// The plugin's arithmetic against the harness's statement of it.
int runModel( int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const Dir* dp : { &kDir625To525, &kDir525To625, &kDir625To525At60, &kDirSame } )
	{
		const Dir& d                 = *dp;
		const model::Conversion conv = conversionOf( d, perturb );

		//Temporal: first field and weights, j from the primed store to far
		//past a day of 59.94.
		int bad = 0, n = 0;
		double worst = 0.0;
		for( int temporal = 0; temporal < model::kTemporalCount; ++temporal )
			for( int64_t j = -40; j < 6000; ++j )
			{
				const Frac p                  = positionOf( j, d );
				const Taps want               = statedTemporal( temporal, p );
				const model::TemporalTaps got = model::TemporalWeights( j, conv, temporal, perturb );
				++n;
				bool ok = got.count == want.count && got.first == want.first;
				for( int k = 0; ok && k < want.count; ++k )
				{
					worst = std::max( worst, std::fabs( got.weight[ k ] - want.w[ k ] ) );
					ok    = std::fabs( got.weight[ k ] - want.w[ k ] ) <= 1e-12;
				}
				if( !ok )
					++bad;
			}
		failures += report( bad == 0, quiet, "model %-18s temporal apertures: %d of %d against the time geometry differ, worst weight %.1e", d.name, bad, n,
		                    worst );

		//Vertical: every destination line, both parities, every tap count,
		//with and without the pre-filter.
		bad = 0;
		n   = 0;
		worst = 0.0;
		for( int taps : { 1, 2, 4, 8 } )
			for( double soft : { 0.0, 0.6 } )
				for( int p = 0; p < 2; ++p )
					for( int m = 0; m < d.dst.lines; ++m )
					{
						const auto want              = statedComposite( taps, soft, sourceLineOf( m, p, d ) );
						const model::LineTaps got    = model::VerticalTaps( m, p, conv, taps, soft, perturb );
						std::map< int64_t, double > g;
						for( int k = 0; k < got.count; ++k )
							g[ got.first + k ] += got.weight[ k ];
						double err = 0.0, sum = 0.0;
						for( const auto& t : want )
						{
							err = std::max( err, std::fabs( g[ t.first ] - t.second ) );
							g.erase( t.first );
						}
						for( const auto& rest : g )
							err = std::max( err, std::fabs( rest.second ) );
						for( int k = 0; k < got.count; ++k )
							sum += got.weight[ k ];
						worst = std::max( worst, err );
						++n;
						if( err > 1e-12 || std::fabs( sum - 1.0 ) > 1e-12 )
							++bad;
					}
		failures += report( bad == 0, quiet, "model %-18s vertical filters: %d of %d lines differ from the stated taps or lose DC, worst %.1e", d.name,
		                    bad, n, worst );
	}

	//50 -> 60 repeats in six exactly; 50 -> 59.94 creeps 1/200 exactly.
	{
		const model::Conversion c60 = conversionOf( kDir625To525At60, perturb );
		int differ = 0;
		for( int64_t j = 0; j < 6000; ++j )
		{
			const model::TemporalTaps a = model::TemporalWeights( j, c60, model::kLinear, perturb );
			const model::TemporalTaps b = model::TemporalWeights( j + 6, c60, model::kLinear, perturb );
			if( b.first != a.first + 5 || std::memcmp( a.weight, b.weight, sizeof( a.weight ) ) != 0 )
				++differ;
		}
		failures += report( differ == 0, quiet, "model 50 -> 60: fields j and j + 6 differ in %d of 6000 (stated: none, bit for bit)", differ );

		const model::Conversion c5994 = conversionOf( kDir625To525, perturb );
		int off = 0;
		for( int64_t j = 0; j < 6000; ++j )
		{
			const model::Position a = model::SourcePositionOf( j, c5994.source.fieldRate, c5994.destination.fieldRate );
			const model::Position b = model::SourcePositionOf( j + 6, c5994.source.fieldRate, c5994.destination.fieldRate );
			//( b - a ) - 5 = 1/200, in the plugin's own integers.
			const int64_t diff = ( b.whole - a.whole - 5 ) * a.den + ( b.rem - a.rem );
			if( a.den != b.den || diff * 200 != a.den )
				++off;
		}
		failures += report( off == 0, quiet, "model 50 -> 59.94: the six-field creep is 1/200 of a field exactly in %d of 6000 cycles", 6000 - off );
	}
	return failures;
}

/// A six-day host clock in milliseconds and a fresh one in seconds must make
/// the same fields at the same frames.
int runClock( int perturb = 0, bool quiet = false )
{
	using standards::Clock;
	using standards::Schedule;
	int failures = 0;
	for( const Dir* dp : { &kDir625To525, &kDir525To625 } )
	{
		const model::Conversion conv = conversionOf( *dp, 0 );
		Clock seconds, millis;
		seconds.SetScaleForTest( 1.0 );
		millis.SetScaleForTest( 0.001 );
		millis.SetFloatForTest( ( perturb & model::kPerturbClockFloat ) != 0 );
		Schedule a, b;
		const double origin = 499000000.0 + 6.0 * 86400.0 * 1000.0;//ms: Resolume's measured clock, plus six days
		int differ = 0, frames = 0;
		std::vector< Schedule::Capture > ca, cb;
		std::vector< int64_t > da, db;
		for( int64_t k = 0; k < 3000; ++k )
		{
			seconds.Update( static_cast< double >( k ) / 60.0 );
			millis.Update( origin + static_cast< double >( k ) * 1000.0 / 60.0 );
			bool pa = false, pb = false;
			a.Frame( seconds.Now(), conv, 0, ca, pa );
			b.Frame( millis.Now(), conv, 0, cb, pb );
			a.Destinations( seconds.Now(), conv, 0, da );
			b.Destinations( millis.Now(), conv, 0, db );
			++frames;
			bool same = ca.size() == cb.size() && da == db;
			for( size_t i = 0; same && i < ca.size(); ++i )
				same = ca[ i ].field == cb[ i ].field && ca[ i ].fromCurrent == cb[ i ].fromCurrent;
			if( !same )
				++differ;
		}
		failures += report( differ == 0, quiet, "clock %-18s a six-day millisecond clock and a fresh seconds clock: %d of %d frames make different fields",
		                    dp->name, differ, frames );
	}

	//A scrub backwards and a stall: the field clock stays monotonic and moves
	//by one nominal frame, so the store does not jump.
	{
		Clock c;
		c.SetScaleForTest( 1.0 );
		double last = -1.0, worstStep = 0.0;
		bool monotonic = true;
		const double times[] = { 10.0, 10.0 + 1 / 60.0, 3.0, 3.0 + 1 / 60.0, 50.0, 50.0 + 1 / 60.0 };
		for( double t : times )
		{
			c.Update( t );
			if( last >= 0.0 )
			{
				monotonic = monotonic && c.Now() > last;
				worstStep = std::max( worstStep, c.Now() - last );
			}
			last = c.Now();
		}
		failures += report( monotonic && worstStep <= 1.0 / 60.0 + 1e-9, quiet,
		                    "clock: a scrub back 7 s and a jump forward 47 s move the field clock by at most %.4f s, forwards", worstStep );
	}
	return failures;
}

int runNames( bool quiet = false )
{
	Standards plugin;
	std::set< std::string > seen;
	int bad = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.name.size() > 16 || !seen.insert( p.name ).second )
			++bad;
	}
	const std::string name = "SW Standards";
	return report( bad == 0 && name.size() <= 16, quiet, "names: %zu parameters, all unique and within 16 characters; the plugin is '%s' (%zu)",
	               seen.size(), name.c_str(), name.size() );
}

//---------------------------------------------------------------------------
// --negative: every check above can fail.
//---------------------------------------------------------------------------
struct NegativeControl
{
	const char* what;
	int failuresSeen;
};

int summariseNegatives( const std::vector< NegativeControl >& controls )
{
	int failures = 0;
	for( const NegativeControl& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		++g_checks;
		std::printf( "negative %-58s %s  %s\n", c.what, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
		{
			++failures;
			++g_failures;
		}
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

/// Run a check against a perturbation quietly, and report whether it failed
/// -- without counting its failures as the run's.
template< typename F >
int caught( F&& check )
{
	const int checks = g_checks, failures = g_failures;
	const int seen   = check();
	g_checks         = checks;
	g_failures       = failures;
	return seen;
}

int runNegativeOffline()
{
	return summariseNegatives( {
		{ "model: temporal weights swapped", caught( [] { return runModel( model::kPerturbSwapWeights, true ); } ) },
		{ "model: line positions ignore the field's parity", caught( [] { return runModel( model::kPerturbIgnoreParity, true ); } ) },
		{ "model: Drop/Repeat takes the field at or before", caught( [] { return runModel( model::kPerturbFloorRepeat, true ); } ) },
		{ "model: 59.94 treated as 60", caught( [] { return runModel( model::kPerturbNoCreep, true ); } ) },
		{ "model: Catmull-Rom detuned to a = -3/4", caught( [] { return runModel( model::kPerturbKernelDetune, true ); } ) },
		{ "clock: kept in float", caught( [] { return runClock( model::kPerturbClockFloat, true ); } ) },
	} );
}

int runNegative( int width, int height )
{
	return summariseNegatives( {
		{ "weights: temporal weights swapped (the spec's)", caught( [ & ] { return runWeights( width, height, model::kPerturbSwapWeights, true ); } ) },
		{ "weights: 59.94 treated as 60 (no creep)", caught( [ & ] { return runWeights( width, height, model::kPerturbNoCreep, true, true ); } ) },
		{ "judder: Drop/Repeat takes the field at or before", caught( [ & ] { return runJudder( width, height, model::kPerturbFloorRepeat, true ); } ) },
		{ "judder: temporal weights swapped", caught( [ & ] { return runJudder( width, height, model::kPerturbSwapWeights, true ); } ) },
		{ "lines: taps ignore the field's parity (the spec's)", caught( [ & ] { return runLines( width, height, model::kPerturbIgnoreParity, true ); } ) },
		{ "lines: Catmull-Rom detuned to a = -3/4", caught( [ & ] { return runLines( width, height, model::kPerturbKernelDetune, true ); } ) },
		{ "same: capture takes the frame at or before", caught( [ & ] { return runSame( width, height, model::kPerturbCaptureBefore, true ); } ) },
		{ "mc: vectors estimated and never applied", caught( [ & ] { return runMc( width, height, model::kPerturbMcZero, true ); } ) },
		{ "mc: vectors applied with the wrong sign", caught( [ & ] { return runMc( width, height, model::kPerturbMcSign, true ); } ) },
		{ "resize: the store re-primed on a resize", caught( [ & ] { return runResize( width, height, model::kPerturbResizeClears, true ); } ) },
	} );
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe.
// It has to move: a converter does nothing visible to a still picture but
// soften it.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t = static_cast< double >( frame ) / 60.0;
	const double barX  = std::fmod( 40.0 + 180.0 * t, static_cast< double >( width ) );
	const double discX = width * ( 0.5 + 0.3 * std::sin( 1.3 * t ) );
	const double discY = height * ( 0.62 + 0.18 * std::sin( 0.9 * t + 1.0 ) );
	const double discR = height * 0.09;
	const int pan      = static_cast< int >( std::floor( 120.0 * t ) );
	const unsigned char bars[ 7 ][ 3 ] = { { 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
		                                   { 191, 0, 191 },   { 191, 0, 0 },   { 0, 0, 191 } };
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fy = ( y + 0.5 ) / height;
			double r = 40 + 60 * fy, g = 70 + 80 * fy, b = 140 + 60 * fy;
			if( fy > 0.08 && fy < 0.22 )
			{
				//Fine horizontal lines: one row on, one off. Interlace food.
				const double v = ( y & 1 ) ? 230.0 : 25.0;
				r = g = b = v;
			}
			else if( fy > 0.26 && fy < 0.40 )
			{
				const unsigned char* c = bars[ std::min( 6, x * 7 / width ) ];
				r = c[ 0 ];
				g = c[ 1 ];
				b = c[ 2 ];
			}
			else if( fy > 0.84 )
			{
				//A panning strip of vertical stripes.
				const int u = ( ( x + pan ) / std::max( 2, width / 64 ) ) & 1;
				r = g = b = u ? 220.0 : 30.0;
			}
			if( std::fabs( x + 0.5 - barX ) < std::max( 2.0, width / 160.0 ) )
				r = g = b = 250.0;
			const double dx = x + 0.5 - discX, dy = y + 0.5 - discY;
			if( dx * dx + dy * dy < discR * discR )
			{
				r = 240;
				g = 190;
				b = 40;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = static_cast< unsigned char >( r );
			px[ 1 ] = static_cast< unsigned char >( g );
			px[ 2 ] = static_cast< unsigned char >( b );
			px[ 3 ] = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is built once per frame of a short loop and uploaded ahead, so
	//the upload is not in the figure; the plugin still sees a moving picture.
	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best = 1e9;
	int64_t frame = warmup;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i, ++frame )
			session.renderAt( frame );
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   %% of a 60fps frame\n" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames );
		std::printf( "%s    %7.3f        %5.1f%%\n", size.name, ms, ms / 16.667 * 100.0 );
	}
	std::printf( "\nAt 60 frames a second a 50-field source makes 5 fields in 6 frames and a\n"
	             "59.94 destination 1 in about 1; each field is one pass. Motion Comp adds a\n"
	             "block search per source pair; run with --set \"Motion Comp=1\" to measure it.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = standards::shaders;
	const std::pair< const char*, const char* > files[] = {
		{ "vertex.vert", sh::kVertex },     { "capture.frag", sh::kCapture }, { "field.frag", sh::kField },
		{ "resample.frag", sh::kResample }, { "motion.frag", sh::kMotion },   { "convert.frag", sh::kConvert },
		{ "display.frag", sh::kDisplay },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"sttest -- render and measure the Standards field-store converter\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/standards.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --weights           each output field's levels are its temporal weights; 6-field cycle; 59.94 creep\n"
		"  --judder            the repeat pattern and the double image, whole-pixel and fractional\n"
		"  --lines             marked lines where the geometry says; impulse and zone-plate response = the taps\n"
		"  --same              Same, Drop/Repeat, one tap: the fielded input, bit for bit\n"
		"  --mc                motion compensation smooths a pan and tears where an occlusion says\n"
		"  --resize            the field store survives a resize mid-run\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --model             the plugin's arithmetic against the stated geometry and kernels\n"
		"  --clock             a six-day millisecond clock makes the same fields as a fresh one\n"
		"  --names             nothing the host will silently truncate\n"
		"  --offline           all three, and their negative controls; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/standards.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );//run the checks verbosely against a perturbed model (Model.h)
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--model", "--clock", "--names", "--negative-offline" } )
				checks.push_back( m );
		else if( argument == "--weights" || argument == "--judder" || argument == "--lines" || argument == "--same"
		         || argument == "--mc" || argument == "--resize" || argument == "--negative" || argument == "--model"
		         || argument == "--clock" || argument == "--names" || argument == "--negative-offline" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Standards plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		//The checks with no GL first; a context only if a rendering one asks.
		bool needGL = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--model" )
				runModel( perturb );
			else if( check == "--clock" )
				runClock( perturb );
			else if( check == "--names" )
				runNames();
			else if( check == "--negative-offline" )
			{
				runNegativeOffline();
				offlineRan = true;
			}
			else
			{
				needGL = true;
				continue;
			}
			std::printf( "\n" );
		}
		if( offlineRan )
			std::printf( "   OFFLINE: --weights, --judder, --lines, --same, --mc, --resize and their negative\n"
			             "   controls were NOT run. Nothing here drew a pixel through a GL driver; the\n"
			             "   shaders were not exercised, only (in CI) compiled by glslc.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--weights" )
						runWeights( width, height, perturb );
					else if( check == "--judder" )
						runJudder( width, height, perturb );
					else if( check == "--lines" )
						runLines( width, height, perturb );
					else if( check == "--same" )
						runSame( width, height, perturb );
					else if( check == "--mc" )
						runMc( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	//--fps as a rational close enough for a clock: 59.94 becomes 60000/1001.
	session.fpsNum = static_cast< int64_t >( std::llround( fps * 1001.0 ) );
	session.fpsDen = 1001;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			//Frame n is clocked at n / fps, never the wall clock: a stall
			//upstream must not show up in the reel as the fields speeding up.
			const bool rendered = index != failRender && session.render( index, frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, buildCard( width, height, frame ) ) )
			return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
