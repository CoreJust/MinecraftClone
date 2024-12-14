#include "BoundsCheck.h"

void Bounds::checkBound(int &pos, int &change) {
	if (pos < 0) {
		pos += 16;
		change -= 1;
	} else if (pos > 15) {
		pos -= 16;
		change += 1;
	}
}

VectorXZ Bounds::getChunk(VectorXZ pos, int &blockX, int &blockZ) {
	static VectorXZ noChange{ 0, 0 };
	VectorXZ change = noChange;

	checkBound(blockX, change.x);
	checkBound(blockZ, change.z);

	return pos + change;
}
