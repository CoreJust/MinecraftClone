#pragma once
#include <glm\glm.hpp>

class Ray final {
public:
	Ray(glm::vec3 &rot, glm::vec3 &pos);

	void step(float scaler);

	glm::vec3& getRayEnd();

private:
	glm::vec3 m_dir;
	glm::vec3 m_rayEnd;
};