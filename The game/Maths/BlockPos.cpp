#include "BlockPos.h"

size_t std::hash<BlockPos>::operator()(const BlockPos &pos) const {
	auto makeHash = [](const BlockPos &p) -> size_t {
		std::hash<int> hasher;
		auto h1 = hasher(p.x);
		auto h2 = hasher(p.y);
		auto h3 = hasher(p.z);

		return std::hash<int>{}(h1 ^ (h2 << h3) ^ h3);
	};

	return makeHash(pos);
}