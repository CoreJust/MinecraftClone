#include "Matrix.h"
#include "../States/State.h"
#include "../Camera.h"

glm::mat4x4 Maths::genViewMatrix(const Camera &cam) {
	glm::mat4x4 r(1.f);
	r = glm::rotate(r, glm::radians(cam.rot.x), { 1, 0, 0 });
	r = glm::rotate(r, glm::radians(cam.rot.y), { 0, 1, 0 });
	r = glm::rotate(r, glm::radians(cam.rot.z), { 0, 0, 1 });

	r = glm::translate(r, -cam.pos);

	return r;
}

glm::mat4x4 Maths::genModelMatrix(const Object &obj, const glm::vec3& startCoords) {
	glm::mat4x4 r(1.f);
	r = glm::translate(r, obj.pos + startCoords);

	r = glm::rotate(r, glm::radians(obj.rot.x), { 1, 0, 0 });
	r = glm::rotate(r, glm::radians(obj.rot.y), { 0, 1, 0 });
	r = glm::rotate(r, glm::radians(obj.rot.z), { 0, 0, 1 });

	r = glm::translate(r, -startCoords);

	return r;
}

glm::mat4x4 Maths::genProjectionMatrix(float fov, float widthToHeight) {
	return glm::perspective(glm::radians(fov), widthToHeight, 0.02f, 1500.f);
}
