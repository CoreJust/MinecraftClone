// Copied from libnoise library

#pragma once
#include <math.h>

namespace noise {
	// Interp funcs
	double cubicInterp(double n0, double n1, double n2, double n3, double a);
	double linearInterp(double n0, double n1, double a);
	double sCurve3(double a);
	double sCurve5(double a);

	// Noise funcs
	enum NoiseQuality {
		QUALITY_FAST = 0,
		QUALITY_STD = 1,
		QUALITY_BEST = 2

	};

	double gradientCoherentNoise3D(double x, double y, double z, int seed = 0,
		NoiseQuality noiseQuality = QUALITY_STD);

	double gradientNoise3D(double fx, double fy, double fz, int ix, int iy,
		int iz, int seed = 0);

	int intValueNoise3D(int x, int y, int z, int seed = 0);

	inline double makeInt32Range(double n) {
		if (n >= 1073741824.0) {
			return (2.0 * fmod(n, 1073741824.0)) - 1073741824.0;
		} else if (n <= -1073741824.0) {
			return (2.0 * fmod(n, 1073741824.0)) + 1073741824.0;
		} else {
			return n;
		}
	}

	double valueCoherentNoise3D(double x, double y, double z, int seed = 0,
		NoiseQuality noiseQuality = QUALITY_STD);

	double valueNoise3D(int x, int y, int z, int seed = 0);
}