#pragma once
#include "Entity.h"
#include "../Camera.h"
#include "../GUI/Inventory.h"

class RenderMaster;

class Player : public Entity {
public:
	Player(int x, int y, int z, sf::Vector2i screenSize);

	void playerInput(World &world, float mouseWheel);
	void update(World &world, float dt) override;
	void draw(RenderMaster &renderer);

	bool isHeadInLiquid(World &world);
	bool isInventoryOpened();

	void openGUI(AdditionalInterface *interface);
	void addItem(Item *item);
	Item* getHeldItem();

	Camera& getCamera();
	glm::vec3 getCameraPos();
	sf::Vector3i& getHitBoxOnLook();

	static constexpr float getLookHeight();

private:
	Inventory m_inventory;
	Camera m_cam;

	sf::Vector3i m_hitBox;
	BlockPos m_destroyingBlock = { 0, -1, 0 };
	int m_destroyLevel = 0;

	bool m_isShowInventory = false;
};