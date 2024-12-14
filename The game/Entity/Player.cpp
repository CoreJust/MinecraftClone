#include "Player.h"
#include "../Maths/Maths.h"
#include "../Maths/Ray.h"
#include "../Textures/TextureAtlas.h"
#include "../GUI/StandartInterface.h"
#include "../Renderer/RenderMaster.h"
#include "../World/World.h"

Player::Player(int x, int y, int z, sf::Vector2i screenSize)
	: Entity({ x, y, z }, { 0.6f, getLookHeight() + 0.15f, 0.6f }), m_cam({ x + 0.3f, y + getLookHeight(), z + 0.3f }, screenSize) { }

void Player::update(World &world, float dt) {
	m_isOnGround = world.isOnGround(m_pos, m_box.getDimensions());
	m_isInLiquid = world.isInLiquid(m_pos, m_box.getDimensions());
	Entity::update();

	m_cam.input(m_velocity, m_isOnGround, m_isInLiquid);
	doCollisionTest(world, dt);

	m_cam.pos = getCameraPos();
	m_cam.update();
	m_box.update(m_pos);

	static float accT = 0;
	accT += dt;

	while (accT >= 0.0055f) {
		float m = (m_isOnGround ? 0.93 : 0.975);
		m_velocity.x *= m;
		m_velocity.z *= m;
		accT -= 0.0055f;
	}
}

void Player::draw(RenderMaster &renderer) {
	m_inventory.draw(renderer, m_isShowInventory);
	renderer.draw(m_hitBox);
	renderer.drawDestroying(m_destroyingBlock, m_destroyLevel);
}

bool Player::isHeadInLiquid(World &world) {
	glm::vec3 head = getCameraPos();
	return world.isInLiquid(head, m_box.getDimensions());
}

bool Player::isInventoryOpened() {
	return m_isShowInventory;
}

void Player::openGUI(AdditionalInterface *interface) {
	m_isShowInventory = true;
	m_inventory.setInterface(interface);
}

void Player::addItem(Item *item) {
	m_inventory.addItem(item);
}

Item* Player::getHeldItem() {
	return m_inventory.getHotBar().getActiveSlot().getItem();
}

Camera& Player::getCamera() {
	return m_cam;
}

glm::vec3 Player::getCameraPos() {
	auto &dims = m_box.getDimensions();
	return m_pos + glm::vec3(dims.x / 2, getLookHeight(), dims.z / 2);
}

sf::Vector3i& Player::getHitBoxOnLook() {
	return m_hitBox;
}

constexpr float Player::getLookHeight() {
	return 1.6f;
}

void Player::playerInput(World &world, float mouseWheel) {
	// Open/close inventory
	static bool wasEPressed = false;
	if (sf::Keyboard::isKeyPressed(sf::Keyboard::E)) {
		if (!wasEPressed) {
			static AdditionalInterface *s_StdInterface = new StandartInterface();

			m_isShowInventory = !m_isShowInventory;
			m_inventory.setInterface(m_isShowInventory ? s_StdInterface : nullptr);
		}

		wasEPressed = true;
	} else {
		wasEPressed = false;
	}

	if (m_isShowInventory) {
		m_inventory.input();
	} else {
		static sf::Clock timer;

		Ray ray(m_cam.rot, m_cam.pos);
		glm::vec3 lastPos;

		m_hitBox = { -1, -1, -1 };
		for (int i = 0; i < 50; ray.step(0.1f)) {
			sf::Vector3i pos = toIntVector(ray.getRayEnd());
			if (pos.y > 255)
				break;

			ItemID block = world.getBlock(pos);
			if (ItemDatabase::isSolid(block)) {
				// Draw hitbox
				m_hitBox = pos;

				// Destroy/set block
				static int timeFromStartDestroy = 0;
				static int blockHardness = 0;

				if (timer.getElapsedTime().asMilliseconds() >= 120) {
					if (sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
						if (timeFromStartDestroy == 0 || pos != m_destroyingBlock) {
							timeFromStartDestroy = clock();
							blockHardness = ItemDatabase::computeHardness(getHeldItem()->getID(), block);
							m_destroyingBlock = pos;
							m_destroyLevel = 1;
						} else if (ItemDatabase::get().getData(block).instrumentType.level != 99) { // block is destroyable
							m_destroyLevel = (clock() - timeFromStartDestroy) / (blockHardness / 5) + 1;
							if (clock() - timeFromStartDestroy >= blockHardness) {
								timeFromStartDestroy = 0;
								m_destroyingBlock = { 0, -1, 0 };
								m_destroyLevel = 0;
								if (getHeldItem()->onDestroyBlock(pos, *this, world))
									timer.restart();
							}
						}
					} else {
						timeFromStartDestroy = 0;
						m_destroyingBlock = { 0, -1, 0 };
						m_destroyLevel = 0;
						if (sf::Mouse::isButtonPressed(sf::Mouse::Right)
							&& timer.getElapsedTime().asMilliseconds() >= 240) {
							ItemID item = (ItemID)m_inventory.getHotBar().getActiveSlot().getItem()->getID();

							ItemDatabase::get().getBlock(world.getBlock(pos))->onRightClick(pos,*this, world);

							fixBlockPosition(lastPos, pos);
							if (ItemDatabase::get().getBlock(item)->onPut(pos, *this, world))
								m_inventory.getHotBar().getActiveSlot().getStack().deleteItem();

							timer.restart();
						} else if (sf::Keyboard::isKeyPressed(sf::Keyboard::B)) { // boom
							int DISTANCE = sf::Keyboard::isKeyPressed(sf::Keyboard::N) ? 20 : 5;

							int px = m_pos.x;
							int py = m_pos.y;
							int pz = m_pos.z;
							for (int x = -DISTANCE; x <= DISTANCE; x++) {
								for (int y = -DISTANCE; y <= DISTANCE; y++) {
									int by = py + y;
									if (by <= 0)
										continue;

									for (int z = -DISTANCE; z <= DISTANCE; z++) {
										if (sqrt(x * x + y * y + z * z) <= DISTANCE)
											world.destroyBlock({ px + x, by, pz + z });
									}
								}
							}

							timer.restart();
						}
					}
				}

				break;
			}

			lastPos = ray.getRayEnd();
			++i;
		}

		// Actions with hotbar
		if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num1))
			m_inventory.getHotBar().setActiveSlot(0);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num2))
			m_inventory.getHotBar().setActiveSlot(1);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num3))
			m_inventory.getHotBar().setActiveSlot(2);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num4))
			m_inventory.getHotBar().setActiveSlot(3);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num5))
			m_inventory.getHotBar().setActiveSlot(4);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num6))
			m_inventory.getHotBar().setActiveSlot(5);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num7))
			m_inventory.getHotBar().setActiveSlot(6);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num8))
			m_inventory.getHotBar().setActiveSlot(7);
		else if (sf::Keyboard::isKeyPressed(sf::Keyboard::Num9))
			m_inventory.getHotBar().setActiveSlot(8);

		static sf::Clock wheelTimer;

		if ((wheelTimer.getElapsedTime().asMilliseconds()) >= 30 && (abs(mouseWheel) >= 1.f)) {
			int change = (mouseWheel < 0 ? -1 : 1);
			m_inventory.getHotBar().setActiveSlot(m_inventory.getHotBar().getActiveSlotNum() - change);
			wheelTimer.restart();
		}
	}
}