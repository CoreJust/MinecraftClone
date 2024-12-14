#pragma once
#include "../Entity.h"

class Zombie : public Entity {
public:
	Zombie(glm::vec3 pos);

	void update(World &world, float dt) override;

	EntityModel* getEntityModel() override;

private:
	EntityModel* m_model;
	bool m_hasObstacle = false;
};