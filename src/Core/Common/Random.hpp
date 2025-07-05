#pragma once

namespace core::common {
	class Random final {
		mutable void* m_rnd;

	public:
		Random();
		~Random();

		void randomSeed();
		void setSeed(unsigned int const seed);
		int randi(int const from, int const to) const;
		float randf(float const from, float const to) const;
		float randNormalized() const; // In range [0, 1)
		float randNormal(float const mean, float const std) const; // Normal distribution

		static Random const& global();
	};
} // namespace core::common
