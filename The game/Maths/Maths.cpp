#include "Maths.h"

int toChunkPos(int p) {
	return (p >= 0 ?
		(p / 16)
		: ((p + 1) / 16 - 1));
}

int abs_pos(int p) {
	return p < 0 ?
		(16 + p) : (p);
}

sf::Vector3i toIntVector(const glm::vec3 &vec) {
	return sf::Vector3i{ (int)floor(vec.x), (int)floor(vec.y), (int)floor(vec.z) };
}

void fixBlockPosition(glm::vec3 &pos, sf::Vector3i &block) {
	static auto getDist = [](float posX, int blockX) -> float {
		float r;
		if (posX < blockX)
			r = float(blockX) - posX;
		else
			r = posX - blockX - 1.f;

		if (r < 0)
			return 0;

		return r;
	};

	float distX = getDist(pos.x, block.x);
	float distY = getDist(pos.y, block.y);
	float distZ = getDist(pos.z, block.z);

	int isFix = 0;
	if (distX > 0)
		isFix++;

	if (distY > 0)
		isFix++;

	if (distZ > 0)
		isFix++;

	if (isFix > 1) {
		float distMax = max(max(distX, distY), distZ);
		if (distX == distMax)
			return void(block = (distX > 0 ? block += { 1, 0, 0 } : block += { -1, 0, 0 }));

		if (distY == distMax)
			return void(block = (distY > 0 ? block += { 0, 1, 0 } : block += { 0, -1, 0 }));

		if (distY == distMax)
			return void(block = (distY > 0 ? block += { 0, 0, 1 } : block += { 0, 0, -1 }));
	} else {
		return void(block = toIntVector(pos));
	}
}

int bilinearInterpolation(int a, int b, int c, int d, int x, int z) {
	int
		width,
		height,
		xDistanceToMax,
		yDistanceToMax,
		xDistanceToMin,
		yDistanceToMin;

	width = 16;
	height = 16;
	xDistanceToMax = 16 - x;
	yDistanceToMax = 16 - z;

	xDistanceToMin = x;
	yDistanceToMin = z;

	return int(1.f / (width * height) *
		float(
			a  * xDistanceToMax * yDistanceToMax +
			c * xDistanceToMin * yDistanceToMax +
			b * xDistanceToMax * yDistanceToMin +
			d * xDistanceToMin * yDistanceToMin
			)
		);
}

int highSmoothedInterpolation(int a, int b, int c, int d, int x, int z) {
	float x_tmp = x / 8.f - 1.f;
	x_tmp = x_tmp * x_tmp * (x_tmp < 0 ? -1.f : 1.f);
	int nx = (x_tmp + 1.f) * 8;

	float z_tmp = z / 8.f - 1.f;
	z_tmp = z_tmp * z_tmp * (z_tmp < 0 ? -1.f : 1.f);
	int nz = (z_tmp + 1.f) * 8;

	return max(bilinearInterpolation(a, b, c, d, x, z), bilinearInterpolation(a, b, c, d, nx, nz));
}
