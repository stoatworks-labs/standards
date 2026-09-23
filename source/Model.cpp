#include "Model.h"

#include <cmath>

namespace standards::model
{

const Standard k625     = { "625/50", 576, 288, { 50, 1 } };
const Standard k525     = { "525/59.94", 480, 240, { 60000, 1001 } };
const Standard k525At60 = { "525/60", 480, 240, { 60, 1 } };

namespace
{
constexpr double kPi = 3.14159265358979323846;

/// Keys' cubic convolution kernel. a = -1/2 is Catmull-Rom: the one cubic
/// that reproduces a quadratic exactly, and the classic four-tap
/// interpolator. -3/4 is the "sharper" variant some hardware used; it is
/// here only as a negative control.
double keys( double x, double a )
{
	x = std::fabs( x );
	if( x < 1.0 )
		return ( ( a + 2.0 ) * x - ( a + 3.0 ) ) * x * x + 1.0;
	if( x < 2.0 )
		return ( ( a * x - 5.0 * a ) * x + 8.0 * a ) * x - 4.0 * a;
	return 0.0;
}

double sinc( double x )
{
	if( x == 0.0 )
		return 1.0;
	const double px = kPi * x;
	return std::sin( px ) / px;
}

Rate reduce( Rate r )
{
	int64_t a = r.num, b = r.den;
	while( b != 0 )
	{
		const int64_t t = a % b;
		a               = b;
		b               = t;
	}
	return { r.num / a, r.den / a };
}
} // namespace

Conversion ConversionOf( int direction, int perturb )
{
	Conversion c;
	switch( direction )
	{
	case kDir525To625:
		c = { k525, k625 };
		break;
	case kDir625To525At60:
		c = { k625, k525At60 };
		break;
	case kDirSame:
		c = { k625, k625 };
		break;
	case kDir625To525:
	default:
		c = { k625, k525 };
		break;
	}

	//The negative control for the creep: a converter that believes 59.94 is
	//60. Everything it does is self-consistent; it just is not 59.94.
	if( perturb & kPerturbNoCreep )
	{
		if( c.source.fieldRate.den == 1001 )
			c.source.fieldRate = { 60, 1 };
		if( c.destination.fieldRate.den == 1001 )
			c.destination.fieldRate = { 60, 1 };
	}
	return c;
}

int64_t FloorDiv( int64_t a, int64_t b )
{
	int64_t q = a / b;
	if( ( a % b ) != 0 && ( a < 0 ) )
		--q;
	return q;
}

int64_t FloorMod( int64_t a, int64_t b )
{
	const int64_t r = a % b;
	return r < 0 ? r + b : r;
}

int FieldParity( int64_t field )
{
	return static_cast< int >( FloorMod( field, 2 ) );
}

double FieldTime( int64_t k, Rate rate )
{
	return static_cast< double >( k ) * static_cast< double >( rate.den ) / static_cast< double >( rate.num );
}

Position SourcePositionOf( int64_t j, Rate source, Rate destination )
{
	//p = j Rd_den Rs_num / ( Rd_num Rs_den ), reduced first so that j times
	//the numerator stays far inside 64 bits for any plausible session.
	const Rate ratio = reduce( { destination.den * source.num, destination.num * source.den } );
	const int64_t numerator = j * ratio.num;
	Position p;
	p.den   = ratio.den;
	p.whole = FloorDiv( numerator, ratio.den );
	p.rem   = numerator - p.whole * ratio.den;
	return p;
}

TemporalTaps TemporalWeights( int64_t j, const Conversion& c, int temporal, int perturb )
{
	const Position p = SourcePositionOf( j, c.source.fieldRate, c.destination.fieldRate );
	const double phi = p.Fraction();

	TemporalTaps t = {};
	t.position     = p.Value();

	switch( temporal )
	{
	case kDropRepeat:
	{
		//The nearest source field; exactly half-way repeats the earlier one,
		//which is what a store that only ever moves its read pointer forward
		//does. Compared in integers: 2 rem > den, not phi > 0.5.
		const bool later = !( perturb & kPerturbFloorRepeat ) && 2 * p.rem > p.den;
		t.count          = 1;
		t.first          = p.whole + ( later ? 1 : 0 );
		t.weight[ 0 ]    = 1.0;
		break;
	}

	case kFourField:
	{
		const double a = ( perturb & kPerturbKernelDetune ) ? -0.75 : -0.5;
		t.count        = 4;
		t.first        = p.whole - 1;
		t.weight[ 0 ]  = keys( 1.0 + phi, a );
		t.weight[ 1 ]  = keys( phi, a );
		t.weight[ 2 ]  = keys( 1.0 - phi, a );
		t.weight[ 3 ]  = keys( 2.0 - phi, a );
		break;
	}

	case kLinear:
	default:
		t.count       = 2;
		t.first       = p.whole;
		t.weight[ 0 ] = 1.0 - phi;
		t.weight[ 1 ] = phi;
		break;
	}

	if( perturb & kPerturbSwapWeights )
		for( int i = 0; i < t.count / 2; ++i )
		{
			const double s                  = t.weight[ i ];
			t.weight[ i ]                   = t.weight[ t.count - 1 - i ];
			t.weight[ t.count - 1 - i ]     = s;
		}

	return t;
}

int64_t RequiredSource( int64_t j, const Conversion& c, int perturb )
{
	( void )perturb;
	return SourcePositionOf( j, c.source.fieldRate, c.destination.fieldRate ).whole + 2;
}

int64_t FirstDestinationFrom( int64_t firstSource, const Conversion& c, int perturb )
{
	//The smallest j with floor( p_j ) - 1 >= firstSource: the four-field
	//aperture's earliest field is then in the store.
	( void )perturb;
	const Rate s = c.source.fieldRate, d = c.destination.fieldRate;
	const Rate ratio = reduce( { d.den * s.num, d.num * s.den } );
	//p_j >= firstSource + 1  <=>  j >= ( firstSource + 1 ) den / num
	const int64_t target = ( firstSource + 1 ) * ratio.den;
	return -FloorDiv( -target, ratio.num );
}

Position SourceLineOf( int m, int p, const Conversion& c, int perturb )
{
	const int64_t ls = c.source.lines;
	const int64_t ld = c.destination.lines;

	//u = ( ( 2m + 1 ) Ls - Ld - 2 p Ld ) / ( 4 Ld ). The parity term is the
	//half-line a bottom field's lines sit below a top field's; leave it out
	//and a bottom field is read as if its lines were a top field's.
	const int64_t parityTerm = ( perturb & kPerturbIgnoreParity ) ? 0 : 2 * p * ld;
	const int64_t numerator  = ( 2 * static_cast< int64_t >( m ) + 1 ) * ls - ld - parityTerm;
	const int64_t den        = 4 * ld;

	Position u;
	u.den   = den;
	u.whole = FloorDiv( numerator, den );
	u.rem   = numerator - u.whole * den;
	return u;
}

double Kernel( int taps, double x, int perturb )
{
	switch( taps )
	{
	case 1:
		//A box, half-open so that exactly one line owns every position:
		//round half up.
		return ( x >= -0.5 && x < 0.5 ) ? 1.0 : 0.0;
	case 2:
		return std::fabs( x ) < 1.0 ? 1.0 - std::fabs( x ) : 0.0;
	case 4:
		return keys( x, ( perturb & kPerturbKernelDetune ) ? -0.75 : -0.5 );
	case 8:
	default:
		return std::fabs( x ) < 4.0 ? sinc( x ) * sinc( x / 4.0 ) : 0.0;
	}
}

LineTaps VerticalTaps( int m, int p, const Conversion& c, int taps, double softness, int perturb )
{
	const Position u = SourceLineOf( m, p, c, perturb );
	const double phi = u.Fraction();

	LineTaps base = {};
	switch( taps )
	{
	case 1:
		base.count       = 1;
		base.first       = static_cast< int >( u.whole + ( 2 * u.rem >= u.den ? 1 : 0 ) );
		base.weight[ 0 ] = 1.0;
		break;

	case 2:
		base.count       = 2;
		base.first       = static_cast< int >( u.whole );
		base.weight[ 0 ] = 1.0 - phi;
		base.weight[ 1 ] = phi;
		break;

	case 4:
		base.count = 4;
		base.first = static_cast< int >( u.whole ) - 1;
		for( int t = 0; t < 4; ++t )
			base.weight[ t ] = Kernel( 4, phi + 1.0 - t, perturb );
		break;

	case 8:
	default:
	{
		base.count = 8;
		base.first = static_cast< int >( u.whole ) - 3;
		if( u.rem == 0 )
		{
			//On a line, every other tap is sin( pi k ) / ( pi k ), which in
			//floating point is 1e-17 and not zero. Zero is what the filter
			//says, and what makes the store read back bitwise.
			base.weight[ 3 ] = 1.0;
			break;
		}
		double sum = 0.0;
		for( int t = 0; t < 8; ++t )
		{
			base.weight[ t ] = Kernel( 8, phi + 3.0 - t, perturb );
			sum += base.weight[ t ];
		}
		//Lanczos taps do not sum to one at a fractional phase; normalised,
		//so a flat field stays flat.
		for( int t = 0; t < 8; ++t )
			base.weight[ t ] /= sum;
		break;
	}
	}

	if( softness <= 0.0 )
		return base;

	//The older boxes' vertical pre-filter: [ a, 1 - 2a, a ] across the field's
	//own lines, convolved in front of the interpolator.
	const double a = 0.25 * softness;
	const double g[ 3 ] = { a, 1.0 - 2.0 * a, a };

	LineTaps out = {};
	out.first    = base.first - 1;
	out.count    = base.count + 2;
	for( int t = 0; t < base.count; ++t )
		for( int d = 0; d < 3; ++d )
			out.weight[ t + d ] += base.weight[ t ] * g[ d ];
	return out;
}

} // namespace standards::model
