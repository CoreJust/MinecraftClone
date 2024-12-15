#include "PerlinNoise.h"
#include <cmath>
#include "Noise.h"

float mul(const sf::Vector2f &a, const sf::Vector2f &b) {
	return a.x * b.x + a.y * b.y;
}

float qunticCurve(float t) {
	return t * t * t * (t * (t * 6 - 15) + 10);
}

float lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

PerlinNoise2D::PerlinNoise2D(int seed, double quantity) : m_quantity(quantity) {
	m_rand.setSeed(seed);
	for (int i = 0; i < 1024; ++i)
		permutationTable[i] = m_rand.randi(-127, 127);
}

float PerlinNoise2D::noise(float fx, float fy) {
	fx *= m_quantity;
	fy *= m_quantity;

	int left = static_cast<int>(floor(fx));
	int top = static_cast<int>(floor(fy));

	float pointInQuadX = fx - left;
	float pointInQuadY = fy - top;

	sf::Vector2f topLeftGradient = randomVector(left, top);
	sf::Vector2f topRightGradient = randomVector(left + 1, top);
	sf::Vector2f bottomLeftGradient = randomVector(left, top + 1);
	sf::Vector2f bottomRightGradient = randomVector(left + 1, top + 1);

	sf::Vector2f distanceToTopLeft = sf::Vector2f{ pointInQuadX, pointInQuadY   };
	sf::Vector2f distanceToTopRight = sf::Vector2f{ pointInQuadX - 1, pointInQuadY   };
	sf::Vector2f distanceToBottomLeft = sf::Vector2f{ pointInQuadX, pointInQuadY - 1 };
	sf::Vector2f distanceToBottomRight = sf::Vector2f{ pointInQuadX - 1, pointInQuadY - 1 };

	float tx1 = mul(distanceToTopLeft, topLeftGradient);
	float tx2 = mul(distanceToTopRight, topRightGradient);
	float bx1 = mul(distanceToBottomLeft, bottomLeftGradient);
	float bx2 = mul(distanceToBottomRight, bottomRightGradient);

	pointInQuadX = qunticCurve(pointInQuadX);
	pointInQuadY = qunticCurve(pointInQuadY);

	float tx = lerp(tx1, tx2, pointInQuadX);
	float bx = lerp(bx1, bx2, pointInQuadX);
	float tb = lerp(tx, bx, pointInQuadY);

	return tb < 0 ? -tb : tb;
}

float PerlinNoise2D::octave_noise(float fx, float fy, int octaves, float persistence) {
	float amplitude = 1;
	float max = 0;
	float result = 0;

	while (octaves-- > 0) {
		max += amplitude;
		result += noise(fx, fy) * amplitude;
		amplitude *= persistence;
		fx *= 2;
		fy *= 2;
	}

	return result / max;
}

sf::Vector2f PerlinNoise2D::randomVector(int x, int y) {
	int v = (int)(((x * 1836311903) ^ (y * 2971215073) + 4807526976) & 1023);
	v = permutationTable[v] & 3;

	switch (v) {
		case 0:  return sf::Vector2f {  1, 0 };
		case 1:  return sf::Vector2f { -1, 0 };
		case 2:  return sf::Vector2f {  0, 1 };
	default: return sf::Vector2f {  0, -1 };
	}
}



PerlinNoise3D::PerlinNoise3D(int seed, float quantity) : m_quantity(quantity) {
	m_rand.setSeed(seed);

	for (int i = 0; i < 256; ++i) {
		p[i] = m_rand.randi(-127, 127);
		m_gx[i] = m_rand.randf(-1.f, 1.f);
		m_gy[i] = m_rand.randf(-1.f, 1.f);
		m_gz[i] = m_rand.randf(-1.f, 1.f);
	}
}

float PerlinNoise3D::noise(float fx, float fy, float fz) {
	fx *= m_quantity;
	fy *= m_quantity;
	fz *= m_quantity;

	int x0 = int(floor(fx));
	int x1 = x0 + 1;
	int y0 = int(floor(fy));
	int y1 = y0 + 1;
	int z0 = int(floor(fz));
	int z1 = z0 + 1;

	float px0 = fx - float(x0);
	float px1 = px0 - 1.0f;
	float py0 = fy - float(y0);
	float py1 = py0 - 1.0f;
	float pz0 = fz - float(z0);
	float pz1 = pz0 - 1.0f;

	int gIndex = p[(x0 + p[(y0 + p[z0 & 255]) & 255]) & 255];
	float d000 = m_gx[gIndex] * px0 + m_gy[gIndex] * py0 + m_gz[gIndex] * pz0;
	gIndex = p[(x1 + p[(y0 + p[z0 & 255]) & 255]) & 255];
	float d001 = m_gx[gIndex] * px1 + m_gy[gIndex] * py0 + m_gz[gIndex] * pz0;

	gIndex = p[(x0 + p[(y1 + p[z0 & 255]) & 255]) & 255];
	float d010 = m_gx[gIndex] * px0 + m_gy[gIndex] * py1 + m_gz[gIndex] * pz0;
	gIndex = p[(x1 + p[(y1 + p[z0 & 255]) & 255]) & 255];
	float d011 = m_gx[gIndex] * px1 + m_gy[gIndex] * py1 + m_gz[gIndex] * pz0;

	gIndex = p[(x0 + p[(y0 + p[z1 & 255]) & 255]) & 255];
	float d100 = m_gx[gIndex] * px0 + m_gy[gIndex] * py0 + m_gz[gIndex] * pz1;
	gIndex = p[(x1 + p[(y0 + p[z1 & 255]) & 255]) & 255];
	float d101 = m_gx[gIndex] * px1 + m_gy[gIndex] * py0 + m_gz[gIndex] * pz1;

	gIndex = p[(x0 + p[(y1 + p[z1 & 255]) & 255]) & 255];
	float d110 = m_gx[gIndex] * px0 + m_gy[gIndex] * py1 + m_gz[gIndex] * pz1;
	gIndex = p[(x1 + p[(y1 + p[z1 & 255]) & 255]) & 255];
	float d111 = m_gx[gIndex] * px1 + m_gy[gIndex] * py1 + m_gz[gIndex] * pz1;

	float wx = ((6 * px0 - 15)*px0 + 10)*px0*px0*px0;
	float wy = ((6 * py0 - 15)*py0 + 10)*py0*py0*py0;
	float wz = ((6 * pz0 - 15)*pz0 + 10)*pz0*pz0*pz0;

	float xa = d000 + wx * (d001 - d000);
	float xb = d010 + wx * (d011 - d010);
	float xc = d100 + wx * (d101 - d100);
	float xd = d110 + wx * (d111 - d110);
	float ya = xa + wy * (xb - xa);
	float yb = xc + wy * (xd - xc);
	float value = ya + wz * (yb - ya);

	return value;
}

float PerlinNoise3D::octave_noise(float fx, float fy, float fz, int octaves, float persistence) {
	float amplitude = 1;
	float max = 0;
	float result = 0;

	while (octaves-- > 0) {
		max += amplitude;
		result += noise(fx, fy, fz) * amplitude;
		amplitude *= persistence;
		fx *= 2;
		fy *= 2;
	}

	return result / max;
}

CoherentPerlinNoise3D::CoherentPerlinNoise3D(int seed, double quantity) : m_seed(seed), m_quantity(quantity) { }

float CoherentPerlinNoise3D::noise(float fx, float fy, float fz) {
	fx *= m_quantity;
	fy *= m_quantity;
	fz *= m_quantity;

	return static_cast<float>(noise::gradientCoherentNoise3D(fx, fy, fz, m_seed));
}

float CoherentPerlinNoise3D::octave_noise(float fx, float fy, float fz, int octaves, float persistence) {
	float amplitude = 1;
	float max = 0;
	float result = 0;

	while (octaves-- > 0) {
		max += amplitude;
		result += noise(fx, fy, fz) * amplitude;
		amplitude *= persistence;
		fx *= 2;
		fy *= 2;
	}

	return result / max;
}
