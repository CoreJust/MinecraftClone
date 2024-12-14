#include "Ray.h"

Ray::Ray(glm::vec3 &rot, glm::vec3 &pos)
	: m_dir(rot), m_rayEnd(pos) { }

void Ray::step(float scaler) {
	float pitch = -glm::radians(m_dir.y + 0);
	float yaw = glm::radians(m_dir.x);
	glm::vec3 direction(
		cos(yaw) * sin(pitch),
		sin(yaw),
		cos(yaw) * cos(pitch)
	);

	m_rayEnd -= direction * scaler;
}

glm::vec3& Ray::getRayEnd() {
	return m_rayEnd;
}
