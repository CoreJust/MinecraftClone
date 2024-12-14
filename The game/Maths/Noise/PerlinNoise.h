#pragma once
#include "../../Utils/Random.h"
#include <SFML\System\Vector2.hpp>

class PerlinNoise2D final {
public:
	PerlinNoise2D(int seed = 0, double quantity = 0.1);

	float noise(float fx, float fy);
	float octave_noise(float fx, float fy, int octaves, float persistence = 0.5f);

private:
	sf::Vector2f randomVector(int x, int y);

private:
	char permutationTable[1024];
	Random<> m_rand;
	double m_quantity;
};

class PerlinNoise3D final {
public:
	PerlinNoise3D(int seed = 0, double quantity = 0.1);

	float noise(float fx, float fy, float fz);
	float octave_noise(float fx, float fy, float fz, int octaves, float persistence = 0.5f);

private:
	char p[256];
	char m_gx[256];
	char m_gy[256];
	char m_gz[256];
	Random<> m_rand;
	double m_quantity;
};

class CoherentPerlinNoise3D final {
public:
	CoherentPerlinNoise3D(int seed = 0, double quantity = 0.1);

	float noise(float fx, float fy, float fz);
	float octave_noise(float fx, float fy, float fz, int octaves, float persistence = 0.5f);

private:
	double m_quantity;
	int m_seed;
};