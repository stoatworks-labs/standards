#include "Schedule.h"

#include <cmath>

namespace standards
{
using namespace model;

double Schedule::Delay( const Conversion& c )
{
	return 2.0 * static_cast< double >( c.source.fieldRate.den ) / static_cast< double >( c.source.fieldRate.num );
}

void Schedule::Frame( double now, const Conversion& c, int perturb, std::vector< Capture >& captures, bool& primedNow )
{
	captures.clear();
	primedNow = false;
	const Rate rs = c.source.fieldRate;

	//The source field due now, in integers from the double clock. The clock
	//is seconds since the first frame, so it is small, and the product is
	//exact to far below a field.
	const int64_t due = static_cast< int64_t >(
	    std::floor( ( now + kEpsilon ) * static_cast< double >( rs.num ) / static_cast< double >( rs.den ) ) );

	if( !primed )
	{
		for( int64_t n = due - kPrimeFields + 1; n <= due; ++n )
			captures.push_back( { n, true } );
		primed       = true;
		primedNow    = true;
		prevTime     = now;
		nextSource   = due + 1;
		newestSource = due;
		nextDest     = FirstDestinationFrom( due - kPrimeFields + 1, c, perturb );
		latestDest   = kNone;
		return;
	}

	if( due - nextSource + 1 > kMaxPerFrame )
		nextSource = due - kMaxPerFrame + 1;

	for( ; nextSource <= due; ++nextSource )
	{
		const double t = FieldTime( nextSource, rs );
		bool fromCurrent;
		if( perturb & kPerturbCaptureBefore )
			fromCurrent = now - t <= kEpsilon;
		else
			//Nearest; a tie goes to the earlier frame.
			fromCurrent = ( now - t ) < ( t - prevTime ) - kEpsilon;
		captures.push_back( { nextSource, fromCurrent } );
		newestSource = nextSource;
	}

	prevTime = now;
}

void Schedule::Destinations( double now, const Conversion& c, int perturb, std::vector< int64_t >& out )
{
	out.clear();
	if( !primed )
		return;

	const Rate rd       = c.destination.fieldRate;
	const double delay  = Delay( c );

	//Released at their own instant plus the latency, and only once the
	//aperture's last field is in the store (a host slower than the source
	//standard can be later than the latency).
	auto ready = [ & ]( int64_t j ) {
		return FieldTime( j, rd ) + delay <= now + kEpsilon && RequiredSource( j, c, perturb ) <= newestSource;
	};

	int64_t last = nextDest - 1;
	while( ready( last + 1 ) )
		++last;
	if( last < nextDest )
		return;

	//Only the newest few are worth rendering after a stall: the display
	//shows the latest pair and nothing reads further back than four.
	if( last - nextDest + 1 > kMaxPerFrame )
		nextDest = last - kMaxPerFrame + 1;

	for( ; nextDest <= last; ++nextDest )
		out.push_back( nextDest );
	latestDest = last;
}

} // namespace standards
