#pragma once
#include <glm\glm.hpp>
#include <glm\gtc\matrix_transform.hpp>

class Camera;
struct Object;

inline glm::vec3 operator*(const glm::vec3 &vec, float num) {
	return glm::vec3{ vec.x * num, vec.y * num, vec.z * num };
};

namespace Maths {
	glm::mat4x4 genViewMatrix(const Camera &cam);
	glm::mat4x4 genModelMatrix(const Object &obj, const glm::vec3& startCoords = { 0, 0, 0 });
	glm::mat4x4 genProjectionMatrix(float fov, float widthToHeight);
}