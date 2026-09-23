#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace standards::controls
{

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

int TapsFromOption( float value )
{
	return model::kTapCounts[ OptionIndex( value, model::kTapOptionCount ) ];
}

double SoftnessFromParam( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}

} // namespace standards::controls
