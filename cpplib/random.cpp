#include "random.h"
#include <cstdlib>

float rnd::uniform(float low, float high)
{
	double normalized = (rand() % 7919) / 7919.0;
	double result = normalized * ((double)high - (double)low) + (double)low;

	return (float)result;
}

Vector3 rnd::uniform_unit_hemisphere()
{
	float y = rnd::uniform(0.0f, 1.0f);
	float r = math::sqrt(1.0f - y * y);
	float phi = rnd::uniform(0.0f, math::PI2);

	float radius = rnd::uniform(0.0f, 1.0f);
	radius = math::sqrt(radius);
	
	Vector3 result;

	result.x = r * math::cos(phi) * radius;
	result.y = y * radius;
	result.z = r * math::sin(phi) * radius;

	return result;
}