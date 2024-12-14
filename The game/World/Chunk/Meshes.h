#pragma once
#include "Mesh.h"

struct Meshes {
	Mesh solidMesh;
	Mesh waterMesh;
	glm::vec3 position;

	Meshes() { }

	void buffer() {
		solidMesh.buffer();
		waterMesh.buffer();
	}

	void reset() {
		solidMesh.reset();
		waterMesh.reset();
	}

	bool hasFaces() {
		return solidMesh.getFacesCount() != 0 || waterMesh.getFacesCount() != 0;
	}
};