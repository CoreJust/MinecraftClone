#pragma once
#include <glm\glm.hpp>
#include <SFML\System\Vector3.hpp>

template <class T>
constexpr T max(T a, T b) {
	return a > b ? a : b;
}

template <class T>
constexpr T min(T a, T b) {
	return a < b ? a : b;
}

template <class T>
constexpr T signed_pow(T a, T b) { // returns pow with saving sign
	int mult = a < 0 ? -1 : 1;
	return pow(abs(a), b) * mult;
}

int toChunkPos(int p);
int abs_pos(int p);
sf::Vector3i toIntVector(const glm::vec3 &vec);

void fixBlockPosition(glm::vec3 &pos, sf::Vector3i &block);
int bilinearInterpolation(int a, int b, int c, int d, int x, int z);
int highSmoothedInterpolation(int a, int b, int c, int d, int x, int z);