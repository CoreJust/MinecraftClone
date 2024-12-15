#include "VectorXZ.h"

VectorXZ operator+(const VectorXZ &a, const VectorXZ &&b) noexcept {
	return VectorXZ{ a.x + b.x, a.z + b.z };
}

VectorXZ operator+(const VectorXZ &a, const VectorXZ &b) noexcept {
	return VectorXZ{ a.x + b.x, a.z + b.z };
}

bool operator==(const VectorXZ &a, const VectorXZ &b) noexcept {
	return a.x == b.x && a.z == b.z;
}

float distance(const VectorXZ& a, const VectorXZ& b) noexcept {
	int x = a.x - b.x;
	int z = a.z - b.z;
	return sqrt(x * x + z * z);
}

std::ostream& std::operator<<(std::ostream &ostr, VectorXZ vec) {
	ostr << vec.x << " : " << vec.z;
	return ostr;
}

size_t std::hash<VectorXZ>::operator()(const VectorXZ& p) const {
	std::hash<int> hasher;
	return std::hash<int>{}((hasher(p.x) ^ hasher(p.z)) >> 2);
}
