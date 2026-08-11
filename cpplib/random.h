#pragma once
#include "maths.h"

#ifdef random
#undef random
#endif

namespace random
{
	float uniform(float low = 0.0f, float high = 1.0f);
	
	// Azimuth: 0 at +x axis in right handed system
	//			pi / 2 at +z axis in right handed system
	// Polar: 0 at the top
	Vector3 uniform_unit_hemisphere();
}