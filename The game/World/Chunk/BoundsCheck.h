#pragma once
#include "../../Maths/VectorXZ.h"

namespace Bounds {
	void checkBound(int &pos, int &change);

	VectorXZ getChunk(VectorXZ pos, int &blockX, int &blockZ);
}