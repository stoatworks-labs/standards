#pragma once

#include "Clock.h"
#include "Model.h"
#include "PassBuffer.h"
#include "Schedule.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Standards -- a field-store standards converter, PAL to NTSC and back, as an
	FFGL effect.

	**The one idea.** Until the 1990s a programme crossing the Atlantic went
	through a box that turned 625-line, 50-field pictures into 525-line,
	59.94-field ones with a store of a few fields and two interpolations: in
	time (each output field from the input fields either side of it, weighted
	by where it falls between them) and in space (each output line from nearby
	input lines of the field being read). Neither knows anything about motion,
	and the look falls out of that: a judder cycle six fields long (50 into 60
	is 5 into 6), creeping at 59.94; double images where an output field lands
	between two input fields; and the soft look of few vertical taps across
	interlaced lines. A crude block-vector motion compensation removes the
	judder on a pan and tears on an occlusion, the way the first
	motion-compensated converters did.

	**Two processors.** The CPU decides everything that is a number
	(`Model.h`: exact field positions, temporal weights, the vertical filter
	table; `Schedule.h`: which fields happen on this host frame). The GPU does
	everything that touches a pixel, by `texelFetch` at integer coordinates
	(`Shaders.h`). See AGENTS.md for the traps and what is verified.
*/
class Standards : public CFFGLPlugin
{
public:
	Standards();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by sttest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// The harness DECLARES its clock unit rather than leaving the voting to
	/// infer one: it renders as fast as the GPU allows.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// The destination fields the last frame displayed: the pair's older and
	/// newer field (Weave), or the shown field twice (Bob).
	void LastDisplayForTest( int64_t& older, int64_t& newer ) const
	{
		older = lastOlder;
		newer = lastNewer;
	}

	/// A field out of the store as floats, line 0 first. False if the store no
	/// longer holds it. Needs the GL context current.
	bool ReadSourceFieldForTest( int64_t field, std::vector< float >& out, int& width, int& lines );
	bool ReadDestinationFieldForTest( int64_t field, std::vector< float >& out, int& width, int& lines );

	/// The block vectors of the last motion pass, (dx, dy) per block.
	bool ReadVectorsForTest( std::vector< float >& out, int& blocksX, int& blocksY );

	const standards::Schedule& ScheduleForTest() const
	{
		return schedule;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Conversion
		PT_DIRECTION,
		PT_TEMPORAL,
		PT_TAPS,
		PT_MOTION,

		//Display
		PT_SHOW_AS,
		PT_OUTPUT_SIZE,

		//Look
		PT_SOFTNESS,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// Source fields the store holds. Four Field reads floor( p ) - 1 ..
	/// floor( p ) + 2 two source fields late, and a host frame can be up to a
	/// frame later still; eight is that with room.
	static constexpr int kSourceRing = 8;
	/// Destination fields held: the display reads the latest three at most.
	static constexpr int kDestRing = 4;

private:
	bool ensureStore( int width, const standards::model::Conversion& c );
	bool resizeStore( int newWidth, const standards::model::Conversion& c );
	void resampleInto( standards::PassBuffer& buffer, int oldWidth, int newWidth, int lines );
	void invalidateStore();
	void uploadTable( const standards::model::Conversion& c, int taps, double softness );

	GLuint sourceTexture( int64_t field ) const;
	GLuint destinationTexture( int64_t field ) const;

	void captureFrame( const FFGLTextureStruct& picture, int lines );
	void makeField( int64_t field, bool fromCurrent );
	void estimateMotion( int64_t pair );
	void convert( int64_t j, const standards::model::Conversion& c, int temporal, bool motion );
	bool readBuffer( standards::PassBuffer& buffer, std::vector< float >& out );

	ffglex::FFGLShader captureShader;
	ffglex::FFGLShader fieldShader;
	ffglex::FFGLShader resampleShader;
	ffglex::FFGLShader motionShader;
	ffglex::FFGLShader convertShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	//The store. Fixed arrays, not vectors: an FFGLFBO has raw GL ids and a
	//user-declared destructor, so a vector reallocation would hand two
	//PassBuffers the same framebuffer and delete it twice.
	standards::PassBuffer lines[ 2 ];///< a host frame on the source lines: this one and the one before
	int linesPrevious = 0;           ///< which of the two holds the previous frame
	standards::PassBuffer source[ kSourceRing ];
	int64_t sourceIndex[ kSourceRing ];
	standards::PassBuffer destination[ kDestRing ];
	int64_t destIndex[ kDestRing ];
	standards::PassBuffer vectors;
	int64_t vectorsPair = 0;
	bool vectorsValid   = false;
	standards::PassBuffer scratch;

	GLuint tableTexture = 0;
	std::string tableKey;

	int storeWidth = 0;
	bool haveConversion = false;
	standards::model::Conversion conversion {};
	int conversionDirection = -1;
	int conversionPerturb   = 0;

	standards::Schedule schedule;
	standards::Clock clock;
	double hostTime = -1.0;
	int clockFrames = 0;

	int64_t lastOlder = 0;
	int64_t lastNewer = 0;

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
