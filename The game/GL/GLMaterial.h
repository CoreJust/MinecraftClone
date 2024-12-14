#pragma once
#include <glm\glm.hpp>

namespace gl {
	struct Material {
		glm::vec3 ambient;
		glm::vec3 diffuse;
		glm::vec3 specular;
		float shineness;
	};
}