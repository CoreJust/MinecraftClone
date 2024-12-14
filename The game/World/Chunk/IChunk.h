#pragma once
#include "../../Maths/VectorXZ.h"
#include "../Items/ItemID.h"

class IChunk {
public:
	IChunk(VectorXZ pos, bool isReal) : m_pos(pos), m_isReal(isReal) { };

	VectorXZ getPos() const {
		return m_pos;
	};

	bool isReal() {
		return m_isReal;
	}

	virtual void makeColumn(ItemID block, int x, int y, int z, int height) = 0;
	virtual void setBlock(ItemID block, int x, int y, int z) = 0;
	virtual void destroyBlock(int x, int y, int z) = 0;

protected:
	VectorXZ m_pos;
	bool m_isReal; // if chunk is real or pre-generated
};