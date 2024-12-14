#pragma once
#include "AABB.h"

struct Plane {
	float distance;
	glm::vec3 normal;

	float distanceTo(const glm::vec3 &point) const;
};

class Frustum final {
public:
	void update(const glm::mat4x4 &mat);

	bool boxInFrustum(const AABB &box) const;

private:
	Plane m_planes[6];
};