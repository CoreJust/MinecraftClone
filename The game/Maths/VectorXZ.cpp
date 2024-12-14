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
