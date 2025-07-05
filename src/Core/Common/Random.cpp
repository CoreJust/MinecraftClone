#include "Random.hpp"
#include <random>
#include <Core/Memory/TypeErasedAccessor.hpp>
#include "Timer.hpp"

namespace core::common {
	using RandomGenerator = std::mt19937;
	using RGAccessor = memory::TypeErasedAccessor<RandomGenerator>;

	Random::Random() 
		: m_rnd(RGAccessor::make()) {
		randomSeed();
	}

	Random::~Random() {
		if (m_rnd) {
			RGAccessor::destroy(m_rnd);
			m_rnd = nullptr;
		}
	}

	void Random::randomSeed() {
		RGAccessor::get(m_rnd).seed(static_cast<unsigned int>(Timer::global().elapsed().ns));
	}

	void Random::setSeed(unsigned int const seed) {
		RGAccessor::get(m_rnd).seed(seed);
	}

	int Random::randi(int const from, int const to) const {
		std::uniform_int_distribution<int> dist(from, to);
		return dist(RGAccessor::get(m_rnd));
	}

	float Random::randf(float const from, float const to) const {
		std::uniform_real_distribution<float> dist(from, to);
		return dist(RGAccessor::get(m_rnd));
	}

	float Random::randNormalized() const {
		std::uniform_real_distribution<float> dist(0.f, 1.f);
		return dist(RGAccessor::get(m_rnd));
	}

	float Random::randNormal(float const mean, float const std) const {
		std::normal_distribution<float> dist(mean, std);
		return dist(RGAccessor::get(m_rnd));
	}

	Random const& Random::global() {
		static thread_local Random s_random;
		return s_random;
	}
} // namespace core::common
