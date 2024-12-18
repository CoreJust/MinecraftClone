#pragma once
#include <algorithm>
#include <ostream>

struct VectorXZ {
	int x;
	int z;
};

VectorXZ operator+(const VectorXZ &a, const VectorXZ &&b) noexcept;
VectorXZ operator+(const VectorXZ &a, const VectorXZ &b) noexcept;
bool operator==(const VectorXZ& a, const VectorXZ& b) noexcept;

float distance(const VectorXZ& a, const VectorXZ& b) noexcept;

namespace std {
	std::ostream& operator<<(std::ostream &ostr, VectorXZ vec);
}

namespace std {
	template<>
	struct hash<VectorXZ> {
		size_t operator() (const VectorXZ& pos) const;
	};
}