#pragma once
#include <ctime>
#include <random>
#include <SFML\System\Vector3.hpp>

template<class RandomGenerator = std::mt19937>
class Random final {
public:
	inline void setSeed(size_t seed) {
		m_rand.seed(seed);
	}

	template<typename T>
	inline T random(T from, T to) {
		std::uniform_int_distribution<int> dist(from, to);
		return dist(m_rand);
	}

	template<typename T>
	inline T random(T from, T to, int seed) {
		setSeed(seed);
		std::uniform_int_distribution<int> dist(from, to);
		return dist(m_rand);
	}

	template <typename T>
	inline static T rand(T from, T to) {
		static Random<RandomGenerator> random;
		return random.random(from, to, clock());
	}

	template<typename T>
	inline static sf::Vector3<T> randChunkCoords(unsigned char(&height)[16][16]) {
		int x = rand(0, 15);
		int z = rand(0, 15);
		int y = rand(0, (int)height[x][z]);
		return sf::Vector3<T> { x, y, z };
	}

private:
	RandomGenerator m_rand;
};