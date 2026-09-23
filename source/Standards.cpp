#include "Standards.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace ffglex;
using namespace standards;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Standards >,// Create method
	"ST01",                    // Plugin unique ID of maximum length 4.
	"SW Standards",            // Plugin name
	2,                         // API major version number
	1,                         // API minor version number
	0,                         // Plugin major version number
	1,                         // Plugin minor version number
	FF_EFFECT,                 // Plugin type
	"A field-store standards converter: 625/50 to 525/59.94 and back, the way programmes crossed the Atlantic before the 1990s.\n\nThe clip becomes fields of the source standard at their own instants. Each output field is interpolated in time from the source fields either side of it, and each output line from nearby lines of the field being read. Neither interpolation knows about motion, so the look falls out: a judder that repeats every six fields (and creeps at 59.94), double images, and a soft picture. Motion Comp adds crude block vectors, which smooth a pan and tear at an occlusion.\n\nStart with something that pans.",// Plugin description
	"Standards FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kDirectionNames[] = { "625/50 > 525/59.94", "525/59.94 > 625/50", "625/50 > 525/60", "Same (no conversion)" };
const char* const kTemporalNames[]  = { "Drop/Repeat", "Linear", "Four Field" };
const char* const kTapNames[]       = { "1", "2", "4", "8" };
const char* const kShowAsNames[]    = { "Weave", "Bob" };
const char* const kOutputNames[]    = { "Native Lines", "Host" };

/// Half float for every field: an 8-bit code value survives the round trip
/// exactly (half's error at 1.0 is 2^-12, an 8-bit step is 2^-8), negative
/// Catmull-Rom lobes survive, and a 4K field is 8.8 MB rather than 17.7.
constexpr GLint kFieldFormat = GL_RGBA16F;

int slotOf( int64_t index, int ring )
{
	return static_cast< int >( model::FloorMod( index, ring ) );
}

int blocksAcross( int width )
{
	return std::max( 1, ( width + shaders::kBlockWidth - 1 ) / shaders::kBlockWidth );
}

int blocksDown( int fieldLines )
{
	return std::max( 1, ( fieldLines + shaders::kBlockLines - 1 ) / shaders::kBlockLines );
}
} // namespace

//---------------------------------------------------------------------------
Standards::Standards()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Fields are instants. They have to be the host's instants, so an export
	//converts the same as the preview.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults: the classic transatlantic direction, a two-field linear
	// interpolator in time and a four-tap one in space, woven onto the
	// host's raster. That is the look the idea is about -- the six-field
	// judder, the double images, the soft picture -- out of the box, with a
	// little of the older boxes' vertical pre-filter.
	//---------------------------------------------------------------------
	params[ PT_DIRECTION ]   = static_cast< float >( model::kDir625To525 );
	params[ PT_TEMPORAL ]    = static_cast< float >( model::kLinear );
	params[ PT_TAPS ]        = 2.0f;//4 taps
	params[ PT_MOTION ]      = 0.0f;
	params[ PT_SHOW_AS ]     = static_cast< float >( controls::kShowWeave );
	params[ PT_OUTPUT_SIZE ] = static_cast< float >( controls::kOutputHost );
	params[ PT_SOFTNESS ]    = 0.25f;
	params[ PT_MIX ]         = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Option lists are in their natural order, not sorted:
	// Direction is the spec's order, Temporal and Vertical Taps are
	// progressions, and the rest are pairs.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};

	declareOptions( PT_DIRECTION, "Direction", kDirectionNames, model::kDirectionCount );
	declareOptions( PT_TEMPORAL, "Temporal", kTemporalNames, model::kTemporalCount );
	declareOptions( PT_TAPS, "Vertical Taps", kTapNames, model::kTapOptionCount );
	SetParamInfo( PT_MOTION, "Motion Comp", FF_TYPE_BOOLEAN, false );

	declareOptions( PT_SHOW_AS, "Show As", kShowAsNames, controls::kShowCount );
	declareOptions( PT_OUTPUT_SIZE, "Output Size", kOutputNames, controls::kOutputCount );

	SetParamInfof( PT_SOFTNESS, "Softness", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_DIRECTION; i <= PT_MOTION; ++i )
		SetParamGroup( i, "Conversion" );
	for( FFUInt32 i = PT_SHOW_AS; i <= PT_OUTPUT_SIZE; ++i )
		SetParamGroup( i, "Display" );
	for( FFUInt32 i = PT_SOFTNESS; i <= PT_MIX; ++i )
		SetParamGroup( i, "Look" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	for( int i = 0; i < kSourceRing; ++i )
		sourceIndex[ i ] = INT64_MIN;
	for( int i = 0; i < kDestRing; ++i )
		destIndex[ i ] = INT64_MIN;

	FFGLLog::LogToHost( "Created Standards effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Standards::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &captureShader, shaders::kCapture, "capture" },    { &fieldShader, shaders::kField, "field" },
		{ &resampleShader, shaders::kResample, "resample" }, { &motionSadShader, shaders::kMotionSad, "motion SAD" },
		{ &motionPickShader, shaders::kMotionPick, "motion pick" },
		{ &convertShader, shaders::kConvert, "convert" },    { &displayShader, shaders::kDisplay, "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Standards: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &tableTexture );
	tableKey.clear();

	invalidateStore();
	schedule.Reset();
	clock.Reset();
	haveConversion = false;
	storeWidth     = 0;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void Standards::invalidateStore()
{
	for( int i = 0; i < kSourceRing; ++i )
		sourceIndex[ i ] = INT64_MIN;
	for( int i = 0; i < kDestRing; ++i )
		destIndex[ i ] = INT64_MIN;
	vectorsValid = false;
}

bool Standards::ensureStore( int width, const model::Conversion& c )
{
	const int ls  = c.source.lines;
	const int lfs = c.source.fieldLines;
	const int lfd = c.destination.fieldLines;

	for( auto& b : lines )
		if( !b.Ensure( width, ls, kFieldFormat, PassBuffer::Sampling::Nearest ) )
			return false;
	for( auto& b : source )
		if( !b.Ensure( width, lfs, kFieldFormat, PassBuffer::Sampling::Nearest ) )
			return false;
	for( auto& b : destination )
		if( !b.Ensure( width, lfd, kFieldFormat, PassBuffer::Sampling::Nearest ) )
			return false;
	const int across = 2 * shaders::kSearchX + 1, down = 2 * shaders::kSearchLines + 1;
	return costs.Ensure( blocksAcross( width ) * across, blocksDown( lfs ) * down, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	       && vectors.Ensure( blocksAcross( width ), blocksDown( lfs ), GL_RGBA32F, PassBuffer::Sampling::Nearest );
}

void Standards::resampleInto( PassBuffer& buffer, int oldWidth, int newWidth, int height )
{
	//Old contents into the scratch buffer at the new width, then the slot
	//reallocated at the new width (which clears it) and the scratch copied
	//back. Two passes, once per resize, and the field survives.
	if( !scratch.Ensure( newWidth, height, kFieldFormat, PassBuffer::Sampling::Nearest ) )
		return;
	{
		ScopedFBOBinding fbo( scratch.GetGLID(), ScopedFBOBinding::RB_REVERT );
		scratch.ResizeViewPort();
		ScopedShaderBinding shader( resampleShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( buffer.TextureID() );
		resampleShader.Set( "Source", 0 );
		resampleShader.Set( "OldWidth", oldWidth );
		resampleShader.Set( "NewWidth", newWidth );
		quad.Draw();
	}
	if( !buffer.Ensure( newWidth, height, kFieldFormat, PassBuffer::Sampling::Nearest ) )
		return;
	{
		ScopedFBOBinding fbo( buffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		buffer.ResizeViewPort();
		ScopedShaderBinding shader( resampleShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( scratch.TextureID() );
		resampleShader.Set( "Source", 0 );
		resampleShader.Set( "OldWidth", newWidth );
		resampleShader.Set( "NewWidth", newWidth );
		quad.Draw();
	}
}

bool Standards::resizeStore( int newWidth, const model::Conversion& c )
{
	//What holds state across frames: the previous host frame's lines (the
	//nearest-frame capture needs it), every source field in the store, every
	//destination field the display may still show. Each is carried to the
	//new width. The other lines buffer is overwritten this frame anyway.
	resampleInto( lines[ linesPrevious ], storeWidth, newWidth, c.source.lines );
	for( int i = 0; i < kSourceRing; ++i )
		if( sourceIndex[ i ] != INT64_MIN )
			resampleInto( source[ i ], storeWidth, newWidth, c.source.fieldLines );
	for( int i = 0; i < kDestRing; ++i )
		if( destIndex[ i ] != INT64_MIN )
			resampleInto( destination[ i ], storeWidth, newWidth, c.destination.fieldLines );
	scratch.Destroy();
	vectorsValid = false;

	diag::info( "store resized from " + std::to_string( storeWidth ) + " to " + std::to_string( newWidth )
	            + " wide, fields kept" );
	return true;
}

void Standards::uploadTable( const model::Conversion& c, int taps, double softness )
{
	char key[ 128 ];
	std::snprintf( key, sizeof( key ), "%d/%d/%d/%d/%.9g/%d", c.source.lines, c.destination.lines, taps, perturb, softness,
	               c.destination.fieldLines );
	if( tableKey == key )
		return;
	tableKey = key;

	//Row ( q * 2 + p ) * Ld/2 + i: destination field line i of parity q, read
	//from a source field of parity p. Three RGBA texels: first line, tap
	//count, ten weights.
	const int lfd = c.destination.fieldLines;
	std::vector< float > table( static_cast< size_t >( 4 * lfd ) * 12, 0.0f );
	for( int q = 0; q < 2; ++q )
		for( int p = 0; p < 2; ++p )
			for( int i = 0; i < lfd; ++i )
			{
				const model::LineTaps t = model::VerticalTaps( 2 * i + q, p, c, taps, softness, perturb );
				float* row              = table.data() + static_cast< size_t >( ( q * 2 + p ) * lfd + i ) * 12;
				row[ 0 ]                = static_cast< float >( t.first );
				row[ 1 ]                = static_cast< float >( t.count );
				for( int k = 0; k < t.count && k < model::kMaxLineTaps; ++k )
					row[ 2 + k ] = static_cast< float >( t.weight[ k ] );
			}

	glBindTexture( GL_TEXTURE_2D, tableTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, 3, 4 * lfd, 0, GL_RGBA, GL_FLOAT, table.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

GLuint Standards::sourceTexture( int64_t field ) const
{
	const int slot = slotOf( field, kSourceRing );
	if( sourceIndex[ slot ] == field )
		return source[ slot ].TextureID();

	//Not in the store -- skipped in a stall, or overwritten. The nearest
	//field that is there is the least wrong picture; black would be a hole.
	int best          = -1;
	int64_t bestDelta = INT64_MAX;
	for( int i = 0; i < kSourceRing; ++i )
	{
		if( sourceIndex[ i ] == INT64_MIN )
			continue;
		const int64_t d = sourceIndex[ i ] > field ? sourceIndex[ i ] - field : field - sourceIndex[ i ];
		if( d < bestDelta )
		{
			bestDelta = d;
			best      = i;
		}
	}
	return best >= 0 ? source[ best ].TextureID() : 0;
}

GLuint Standards::destinationTexture( int64_t field ) const
{
	const int slot = slotOf( field, kDestRing );
	if( destIndex[ slot ] == field )
		return destination[ slot ].TextureID();
	const int64_t latest = schedule.LatestDestination();
	const int latestSlot = slotOf( latest, kDestRing );
	return destIndex[ latestSlot ] == latest ? destination[ latestSlot ].TextureID() : 0;
}

//---------------------------------------------------------------------------
void Standards::captureFrame( const FFGLTextureStruct& picture, int frameLines )
{
	PassBuffer& target = lines[ 1 - linesPrevious ];
	ScopedFBOBinding fbo( target.GetGLID(), ScopedFBOBinding::RB_REVERT );
	target.ResizeViewPort();
	ScopedShaderBinding shader( captureShader.GetGLID() );
	ScopedSamplerActivation sampler( 0 );
	Scoped2DTextureBinding texture( picture.Handle );
	captureShader.Set( "InputTexture", 0 );
	captureShader.Set( "InWidth", static_cast< int >( picture.Width ) );
	captureShader.Set( "InHeight", static_cast< int >( picture.Height ) );
	captureShader.Set( "Width", storeWidth );
	captureShader.Set( "Lines", frameLines );
	quad.Draw();
}

void Standards::makeField( int64_t field, bool fromCurrent )
{
	const int slot       = slotOf( field, kSourceRing );
	const PassBuffer& in = fromCurrent ? lines[ 1 - linesPrevious ] : lines[ linesPrevious ];

	ScopedFBOBinding fbo( source[ slot ].GetGLID(), ScopedFBOBinding::RB_REVERT );
	source[ slot ].ResizeViewPort();
	ScopedShaderBinding shader( fieldShader.GetGLID() );
	ScopedSamplerActivation sampler( 0 );
	Scoped2DTextureBinding texture( in.TextureID() );
	fieldShader.Set( "FrameLines", 0 );
	fieldShader.Set( "Parity", model::FieldParity( field ) );
	quad.Draw();

	sourceIndex[ slot ] = field;
}

void Standards::estimateMotion( int64_t pair )
{
	if( vectorsValid && vectorsPair == pair )
		return;

	{
		ScopedFBOBinding fbo( costs.GetGLID(), ScopedFBOBinding::RB_REVERT );
		costs.ResizeViewPort();
		ScopedShaderBinding shader( motionSadShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding a( sourceTexture( pair ) );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding b( sourceTexture( pair + 1 ) );

		motionSadShader.Set( "FieldA", 0 );
		motionSadShader.Set( "FieldB", 1 );
		motionSadShader.Set( "ParityA", model::FieldParity( pair ) );
		motionSadShader.Set( "ParityB", model::FieldParity( pair + 1 ) );
		motionSadShader.Set( "Width", storeWidth );
		motionSadShader.Set( "FieldLines", conversion.source.fieldLines );
		motionSadShader.Set( "BlockWidth", shaders::kBlockWidth );
		motionSadShader.Set( "BlockLines", shaders::kBlockLines );
		motionSadShader.Set( "SearchX", shaders::kSearchX );
		motionSadShader.Set( "SearchLines", shaders::kSearchLines );
		motionSadShader.Set( "Lambda", 1e-3f );
		quad.Draw();
	}
	{
		ScopedFBOBinding fbo( vectors.GetGLID(), ScopedFBOBinding::RB_REVERT );
		vectors.ResizeViewPort();
		ScopedShaderBinding shader( motionPickShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding c( costs.TextureID() );
		motionPickShader.Set( "Costs", 0 );
		motionPickShader.Set( "SearchX", shaders::kSearchX );
		motionPickShader.Set( "SearchLines", shaders::kSearchLines );
		quad.Draw();
	}

	vectorsPair  = pair;
	vectorsValid = true;
}

void Standards::convert( int64_t j, const model::Conversion& c, int temporal, bool motion )
{
	const model::TemporalTaps taps = model::TemporalWeights( j, c, temporal, perturb );
	const int q                    = model::FieldParity( j );
	const int lfd                  = c.destination.fieldLines;

	//Motion is estimated between the bracketing pair, floor( p ) and the
	//field after it, and applied to every field of the aperture.
	const bool useMotion = motion && !( perturb & model::kPerturbMcZero );
	if( motion )
		estimateMotion( static_cast< int64_t >( std::floor( taps.position ) ) );

	const int slot = slotOf( j, kDestRing );
	ScopedFBOBinding fbo( destination[ slot ].GetGLID(), ScopedFBOBinding::RB_REVERT );
	destination[ slot ].ResizeViewPort();
	ScopedShaderBinding shader( convertShader.GetGLID() );

	int rows[ 4 ]      = { 0, 0, 0, 0 };
	float weights[ 4 ] = { 0, 0, 0, 0 };
	float offsets[ 4 ] = { 0, 0, 0, 0 };
	GLuint textures[ 4 ] = { 0, 0, 0, 0 };
	for( int k = 0; k < taps.count; ++k )
	{
		const int64_t n = taps.first + k;
		rows[ k ]       = ( q * 2 + model::FieldParity( n ) ) * lfd;
		weights[ k ]    = static_cast< float >( taps.weight[ k ] );
		offsets[ k ]    = static_cast< float >( static_cast< double >( n ) - taps.position );
		textures[ k ]   = sourceTexture( n );
	}
	//Unused samplers still need a texture bound: the field that is there.
	for( int k = taps.count; k < 4; ++k )
		textures[ k ] = textures[ 0 ];

	ScopedSamplerActivation s0( 0 );
	Scoped2DTextureBinding t0( textures[ 0 ] );
	ScopedSamplerActivation s1( 1 );
	Scoped2DTextureBinding t1( textures[ 1 ] );
	ScopedSamplerActivation s2( 2 );
	Scoped2DTextureBinding t2( textures[ 2 ] );
	ScopedSamplerActivation s3( 3 );
	Scoped2DTextureBinding t3( textures[ 3 ] );
	ScopedSamplerActivation s4( 4 );
	Scoped2DTextureBinding table( tableTexture );
	ScopedSamplerActivation s5( 5 );
	Scoped2DTextureBinding vectorTexture( vectors.TextureID() );

	convertShader.Set( "Field0", 0 );
	convertShader.Set( "Field1", 1 );
	convertShader.Set( "Field2", 2 );
	convertShader.Set( "Field3", 3 );
	convertShader.Set( "Table", 4 );
	convertShader.Set( "Vectors", 5 );
	convertShader.Set( "Row0", rows[ 0 ] );
	convertShader.Set( "Row1", rows[ 1 ] );
	convertShader.Set( "Row2", rows[ 2 ] );
	convertShader.Set( "Row3", rows[ 3 ] );
	convertShader.Set( "Count", taps.count );
	convertShader.Set( "Weights", weights[ 0 ], weights[ 1 ], weights[ 2 ], weights[ 3 ] );
	convertShader.Set( "Offsets", offsets[ 0 ], offsets[ 1 ], offsets[ 2 ], offsets[ 3 ] );
	convertShader.Set( "Width", storeWidth );
	convertShader.Set( "SourceLines", c.source.fieldLines );
	convertShader.Set( "DestLines", lfd );
	convertShader.Set( "UseMC", useMotion ? 1 : 0 );
	convertShader.Set( "BlocksX", blocksAcross( storeWidth ) );
	convertShader.Set( "BlocksY", blocksDown( c.source.fieldLines ) );
	convertShader.Set( "BlockWidth", shaders::kBlockWidth );
	convertShader.Set( "BlockLines", shaders::kBlockLines );
	convertShader.Set( "VectorSign", ( perturb & model::kPerturbMcSign ) ? -1.0f : 1.0f );
	quad.Draw();

	destIndex[ slot ] = j;
}

//---------------------------------------------------------------------------
FFResult Standards::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	clock.SetFloatForTest( ( perturb & model::kPerturbClockFloat ) != 0 );
	clock.Update( hostTime );
	const double now = clock.Now();
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale="
		            + std::to_string( clock.ClockScale() ) + " seconds=" + std::to_string( now ) );

	const int direction  = controls::OptionIndex( params[ PT_DIRECTION ], model::kDirectionCount );
	const int temporal   = controls::OptionIndex( params[ PT_TEMPORAL ], model::kTemporalCount );
	const int taps       = controls::TapsFromOption( params[ PT_TAPS ] );
	const bool motion    = params[ PT_MOTION ] >= 0.5f;
	const int showAs     = controls::OptionIndex( params[ PT_SHOW_AS ], controls::kShowCount );
	const int outputSize = controls::OptionIndex( params[ PT_OUTPUT_SIZE ], controls::kOutputCount );
	const double soft    = controls::SoftnessFromParam( params[ PT_SOFTNESS ] );

	const int width = static_cast< int >( picture.Width );

	//---------------------------------------------------------------------
	// The standards. A change of direction is a different machine: the
	// store's line structure changes, so it is emptied and primed again.
	//---------------------------------------------------------------------
	const int creepBits = perturb & model::kPerturbNoCreep;
	if( !haveConversion || direction != conversionDirection || creepBits != conversionPerturb )
	{
		conversion          = model::ConversionOf( direction, perturb );
		conversionDirection = direction;
		conversionPerturb   = creepBits;
		haveConversion      = true;
		schedule.Reset();
		invalidateStore();
	}
	const model::Conversion& c = conversion;

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//
	// A change of width keeps the store: every field is carried to the new
	// width. A store that cleared on a resize would show the wrong picture
	// for the whole aperture after every resize.
	//---------------------------------------------------------------------
	if( storeWidth != 0 && width != storeWidth && schedule.Primed() )
	{
		if( perturb & model::kPerturbResizeClears )
		{
			schedule.Reset();
			invalidateStore();
		}
		else
			resizeStore( width, c );
	}
	if( !ensureStore( width, c ) )
	{
		diag::error( "could not allocate the field store at " + std::to_string( width ) + " wide" );
		return FF_FAIL;
	}
	storeWidth = width;
	uploadTable( c, taps, soft );

	//---------------------------------------------------------------------
	// 1. This host frame onto the source lines.
	//---------------------------------------------------------------------
	captureFrame( picture, c.source.lines );

	//---------------------------------------------------------------------
	// 2. Source fields due since the last frame, each from the nearer of
	//    this frame and the one before.
	//---------------------------------------------------------------------
	std::vector< Schedule::Capture > captures;
	bool primedNow = false;
	schedule.Frame( now, c, perturb, captures, primedNow );
	if( primedNow )
		invalidateStore();
	for( const Schedule::Capture& capture : captures )
		makeField( capture.field, capture.fromCurrent );
	linesPrevious = 1 - linesPrevious;

	//---------------------------------------------------------------------
	// 3. Destination fields due, at their own instants.
	//---------------------------------------------------------------------
	std::vector< int64_t > due;
	schedule.Destinations( now, c, perturb, due );
	for( int64_t j : due )
		convert( j, c, temporal, motion );

	if( !schedule.HasDestination() )
		return FF_FAIL;

	//---------------------------------------------------------------------
	// 4. Display. Weave: the latest complete pair (2i, 2i + 1), one field of
	//    latency, which is what a real monitor of an interlaced signal has.
	//    Bob: the latest field.
	//---------------------------------------------------------------------
	const int64_t latest = schedule.LatestDestination();
	int64_t older = latest, newer = latest;
	if( showAs == controls::kShowWeave )
	{
		if( model::FieldParity( latest ) == 1 )
		{
			older = latest - 1;
			newer = latest;
		}
		else
		{
			older = latest - 2;
			newer = latest - 1;
		}
	}
	lastOlder = older;
	lastNewer = newer;

	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding a( destinationTexture( older ) );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding b( destinationTexture( newer ) );
		ScopedSamplerActivation s2( 2 );
		Scoped2DTextureBinding input( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		displayShader.Set( "FieldA", 0 );
		displayShader.Set( "FieldB", 1 );
		displayShader.Set( "InputTexture", 2 );
		displayShader.Set( "ParityA", model::FieldParity( older ) );
		displayShader.Set( "ParityB", model::FieldParity( newer ) );
		displayShader.Set( "ShowAs", showAs );
		displayShader.Set( "OutputSize", outputSize );
		displayShader.Set( "NativeOffset",
		                   static_cast< int >( model::FloorDiv( hostViewport[ 3 ] - c.destination.lines, 2 ) ) );
		displayShader.Set( "VpX", hostViewport[ 0 ] );
		displayShader.Set( "VpY", hostViewport[ 1 ] );
		displayShader.Set( "VpW", hostViewport[ 2 ] );
		displayShader.Set( "VpH", hostViewport[ 3 ] );
		displayShader.Set( "Width", storeWidth );
		displayShader.Set( "FrameLines", c.destination.lines );
		displayShader.Set( "FieldLines", c.destination.fieldLines );
		displayShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		displayShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Standards::DeInitGL()
{
	captureShader.FreeGLResources();
	fieldShader.FreeGLResources();
	resampleShader.FreeGLResources();
	motionSadShader.FreeGLResources();
	motionPickShader.FreeGLResources();
	convertShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();

	for( auto& b : lines )
		b.Destroy();
	for( auto& b : source )
		b.Destroy();
	for( auto& b : destination )
		b.Destroy();
	costs.Destroy();
	vectors.Destroy();
	scratch.Destroy();

	if( tableTexture != 0 )
	{
		glDeleteTextures( 1, &tableTexture );
		tableTexture = 0;
	}
	tableKey.clear();
	storeWidth     = 0;
	haveConversion = false;
	invalidateStore();
	schedule.Reset();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
bool Standards::readBuffer( PassBuffer& buffer, std::vector< float >& out )
{
	if( !buffer.IsValid() )
		return false;
	const int w = static_cast< int >( buffer.GetWidth() );
	const int h = static_cast< int >( buffer.GetHeight() );
	out.assign( static_cast< size_t >( w ) * h * 4, 0.0f );
	ScopedFBOBinding fbo( buffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, w, h, GL_RGBA, GL_FLOAT, out.data() );
	return true;
}

bool Standards::ReadSourceFieldForTest( int64_t field, std::vector< float >& out, int& width, int& fieldLines )
{
	const int slot = slotOf( field, kSourceRing );
	if( sourceIndex[ slot ] != field )
		return false;
	width      = static_cast< int >( source[ slot ].GetWidth() );
	fieldLines = static_cast< int >( source[ slot ].GetHeight() );
	return readBuffer( source[ slot ], out );
}

bool Standards::ReadDestinationFieldForTest( int64_t field, std::vector< float >& out, int& width, int& fieldLines )
{
	const int slot = slotOf( field, kDestRing );
	if( destIndex[ slot ] != field )
		return false;
	width      = static_cast< int >( destination[ slot ].GetWidth() );
	fieldLines = static_cast< int >( destination[ slot ].GetHeight() );
	return readBuffer( destination[ slot ], out );
}

bool Standards::ReadVectorsForTest( std::vector< float >& out, int& blocksX, int& blocksY )
{
	if( !vectorsValid )
		return false;
	blocksX = static_cast< int >( vectors.GetWidth() );
	blocksY = static_cast< int >( vectors.GetHeight() );
	return readBuffer( vectors, out );
}

//---------------------------------------------------------------------------
FFResult Standards::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Standards::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Standards::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Standards::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Standards::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
