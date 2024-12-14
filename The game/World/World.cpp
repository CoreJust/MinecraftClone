#include "World.h"
#include "../Entity/Mobs/Zombie.h"
#include "../Maths/Maths.h"

constexpr int SAFE_DISTANCE = 16;

void World::generate(int pX, int pZ) {
	int SRD = min(m_RD, 3);

	int chunkX = toChunkPos(pX);
	int chunkZ = toChunkPos(pZ);

	for (int x = -SRD + chunkX; x <= SRD + chunkX; ++x) {
		for (int z = -SRD + chunkZ; z <= SRD + chunkZ; ++z) {
			VectorXZ pos{ x, z };

			m_chunksMap.emplace(pos, Chunk(*this, pos));
			m_generator.generateTerrain(m_chunksMap[pos]);
		}
	}

	for (auto &it : m_chunksMap)
		it.second.makeMesh();
}

void World::update(glm::vec3 &camPos, float dt) {
	// Update time
	m_worldTime += dt;

	// Update entities
	updateEntities(dt);

	int player_chunk_x = toChunkPos(camPos.x);
	int player_chunk_z = toChunkPos(camPos.z);

	// Delete chunks out of render distance
	for (auto &it : m_chunksMap) {
		auto pos = it.second.getPos();
		if (max(abs(pos.x - player_chunk_x), abs(pos.z - player_chunk_z)) > m_RD + 1)
			addChunkToDelete(pos);
	}

	for (auto &it : m_preChunksMap) {
		auto pos = it.second.getPos();
		if (max(abs(pos.x - player_chunk_x), abs(pos.z - player_chunk_z)) > m_RD + 2)
			m_preChunksMap.erase(pos);
	}

	if (m_chunksToDelete.size())
		deleteChunks();

	// Loading new chunks
	for (int x = -m_RD; x <= m_RD; ++x) {
		for (int z = -m_RD; z <= m_RD; ++z) {
			VectorXZ pos{ x + player_chunk_x, z + player_chunk_z };

			if (m_chunksMap.find(pos) == m_chunksMap.end()) {
				addChunkToLoad(pos);
				continue;
			}
		}
	}

	if (!m_chunksToLoad.empty())
		loadChunks();

	// Regenerate chunks meshes
	if (m_isRegenChunks) {
		m_isRegenChunks = false;
		for (auto &it : m_chunksMap) {
			if (it.second.regenFlag) {
				it.second.makeMesh();
				it.second.regenFlag = false;
			}
		}
	}
}

void World::updateEntities(float dt) {
	int px = m_player->getX();
	int pz = m_player->getZ();
	if (isNight() && m_entities.size() < 16) {
		static Random<> r;
		int x, z;
		do {
			x = r.random(px - m_spawnDistance, px + m_spawnDistance);
			z = r.random(pz - m_spawnDistance, pz + m_spawnDistance);
		} while (abs(px - x) < SAFE_DISTANCE || abs(pz - z) < SAFE_DISTANCE);

		int y = ((Chunk*)getChunk({ toChunkPos(x), toChunkPos(z) }))->heightMap[abs_pos(x % 16)][abs_pos(z % 16)];

		summon(new Zombie({ x, y, z }));
	}

	for (auto &e : m_entities)
		e->update(*this, dt);
}

void World::tick() {
	std::vector<BlockPos> updatedBlocks;
	for (auto u = m_updatables.begin(); u != m_updatables.end(); ++u) {
		if ((u->second -= 0.1f) <= 0.f) {
			ItemDatabase::get().getBlock(getBlock(u->first))->onUpdate(u->first, *this);
			updatedBlocks.push_back(u->first);
		}
	}

	for (auto &u : updatedBlocks)
		m_updatables.erase(u);
}

int World::draw(RenderMaster &renderer, Camera &cam) {
	// Draw mobs
	for (auto &e : m_entities)
		renderer.draw(e);

	// Draw world
	int r = 0;
	for (auto &it : m_chunksMap) {
		auto &chunk = it.second;
		r += chunk.draw(cam, renderer);
	}

	return r;
}

std::unordered_map<VectorXZ, Chunk>& World::getChunks() {
	return m_chunksMap;
}

IChunk* World::getChunk(VectorXZ pos) {
	if (m_chunksMap.find(pos) != m_chunksMap.end())
		return &m_chunksMap[pos];

	return &m_preChunksMap[pos];
}

bool World::isOnGround(glm::vec3 &pos, const glm::vec3 &box) {
	if (pos.y != floor(pos.y))
		return false;

	int start = (int)floor(pos.y - 1.f);
	sf::Vector3i blocks[4];
	blocks[0] = { (int)floor(pos.x),		  start, (int)floor(pos.z) };
	blocks[1] = { (int)floor(pos.x + box.x),  start, (int)floor(pos.z) };
	blocks[2] = { (int)floor(pos.x),		  start, (int)floor(pos.z + box.z) };
	blocks[3] = { (int)floor(pos.x + box.x),  start, (int)floor(pos.z + box.z) };

	for (int i = 0; i < 4; ++i) {
		if (ItemDatabase::isSolid(getBlock(blocks[i])))
			return true;
	}

	return false;
}

bool World::isInLiquid(glm::vec3 &pos, const glm::vec3 &box) {
	int start = (int)floor(pos.y);
	sf::Vector3i blocks[4];
	blocks[0] = { (int)floor(pos.x),		  start, (int)floor(pos.z) };
	blocks[1] = { (int)floor(pos.x + box.x),  start, (int)floor(pos.z) };
	blocks[2] = { (int)floor(pos.x),		  start, (int)floor(pos.z + box.z) };
	blocks[3] = { (int)floor(pos.x + box.x),  start, (int)floor(pos.z + box.z) };

	for (int k = 0; k < 2; ++k) {
		for (int i = 0; i < 4; ++i) {
			if (ItemDatabase::isLiquid(getBlock(blocks[i])))
				return true;

			blocks[i].y += (pos.y - floor(pos.y) >= 0.4f ? 1 : 0);
		}
	}

	return false;
}
void World::destroyBlock(const BlockPos &pos) {
	VectorXZ chunkPos{ toChunkPos(pos.x), toChunkPos(pos.z) };
	if (m_chunksMap.find(chunkPos) != m_chunksMap.end()) {
		m_chunksMap[chunkPos].destroyBlock(abs_pos(pos.x % 16), pos.y, abs_pos(pos.z % 16));
		addUpdatablesFor(pos);
		addChunksToRegenMesh(chunkPos);
	}
}

bool World::trySetBlock(ItemID block, const BlockPos &pos, Player &player) {
	VectorXZ chunkPos{ toChunkPos(pos.x), toChunkPos(pos.z) };
	if (player.getAABB().isColideWith(AABB::makeBlockBox(pos))) // check if block not collide witl player
		return false;

	if (m_chunksMap.find(chunkPos) != m_chunksMap.end()) { // check if chunk not exists
		auto &chunk = m_chunksMap[chunkPos];
		int blockX = abs_pos(pos.x % 16);
		int blockZ = abs_pos(pos.z % 16);

		if (hasNeighbours(chunk, { blockX, pos.y, blockZ })) {
			m_chunksMap[chunkPos].setBlock(block, blockX, pos.y, blockZ);
			addUpdatablesFor(pos);
			addChunksToRegenMesh(chunkPos);
		}
	}

	return true;
}

void World::setBlock(ItemID block, const BlockPos &pos) {
	VectorXZ chunkPos{ toChunkPos(pos.x), toChunkPos(pos.z) };
	if (m_chunksMap.find(chunkPos) != m_chunksMap.end()) {
		auto &chunk = m_chunksMap[chunkPos];
		int blockX = abs_pos(pos.x % 16);
		int blockZ = abs_pos(pos.z % 16);

		m_chunksMap[chunkPos].setBlock(block, blockX, pos.y, blockZ);

		addUpdatablesFor(pos);
		addChunksToRegenMesh(chunkPos);
	}
}

void World::addUpdatablesFor(const BlockPos &pos) {
	static sf::Vector3i offsets[] = {
		{ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
		{ -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 },
		{ 0, 0, 0 }
	};

	for (int i = 0; i < 7; ++i) {
		sf::Vector3i p = pos + offsets[i];
		auto id = getBlock(p);
		if (ItemDatabase::isUpdatable(id))
			m_updatables[p] = ItemDatabase::get().getBlock(id)->timeToUpdate();
	}
}

void World::fillBlocks(ItemID block, int x, int sizeX, int y, int sizeY, int z, int sizeZ) {
	int x1 = x + sizeX;
	int y1 = y + sizeY;
	int z1 = z + sizeZ;

	for (int bX = x; bX < x1; ++bX) {
		for (int bY = y; bY < y1; ++bY) {
			for (int bZ = z; bZ < z1; ++bZ) {
				setBlock(block, { bX, bY, bZ });
			}
		}
	}
}

void World::destroyBlocks(int x, int sizeX, int y, int sizeY, int z, int sizeZ) {
	int x1 = x + sizeX;
	int y1 = y + sizeY;
	int z1 = z + sizeZ;

	for (int bX = x; bX < x1; ++bX) {
		for (int bY = y; bY < y1; ++bY) {
			for (int bZ = z; bZ < z1; ++bZ) {
				destroyBlock({ bX, bY, bZ });
			}
		}
	}
}

ItemID World::getBlock(int x, int y, int z) const {
	VectorXZ pos { toChunkPos(x), toChunkPos(z) };
	if (m_chunksMap.find(pos) == m_chunksMap.end())
		return ItemID::AIR;

	return m_chunksMap.at(pos).at(abs_pos(x % 16), y, abs_pos(z % 16));
}

ItemID World::getBlock(const BlockPos &pos) const {
	return getBlock(pos.x, pos.y, pos.z);
}

bool World::hasNeighbours(Chunk &chunk, const BlockPos &pos) const {
	static sf::Vector3i offsets[] = {
		{ 1, 0, 0 },  { 0, 1, 0 },  { 0, 0, 1 },
		{ -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 }
	};

	for (int i = 0; i < 6; ++i)
		if (chunk.getBlock(pos.x + offsets[i].x, pos.y + offsets[i].y, pos.z + offsets[i].z))
			return true;

	return false;
}

void World::summon(Entity *entity) {
	m_entities.push_back(entity);
}

void World::setPlayer(Player *p) {
	m_player = p;
}

Player* World::getPlayer() {
	return m_player;
}

void World::setRD(int rd) {
	m_RD = rd;
	m_spawnDistance = std::min(64, 16 * m_RD);
}

int World::getRD() const {
	return m_RD;
}

int World::getWorldDay() const {
	return int(m_worldTime) / DAY_SIZE;
}

float World::getWorldTime() const {
	return m_worldTime - getWorldDay() * DAY_SIZE;
}

bool World::isDay() const {
	return getWorldTime() > DAY_START && getWorldTime() < DAY_END;
}

bool World::isNight() const {
	return getWorldTime() > NIGHT_START && getWorldTime() < NIGHT_END;
}

bool World::isMorning() const {
	return getWorldTime() > NIGHT_END || getWorldTime() < DAY_START;
}

bool World::isEvening() const {
	return getWorldTime() > DAY_END && getWorldTime() < NIGHT_START;
}

const Random<std::mt19937>& World::getRandom() const {
	static Random<std::mt19937> r;
	return r;
}

void World::loadChunks() {
	static sf::Clock timer;
	static int unfinished = 0;

	if (timer.getElapsedTime().asMilliseconds() >= 30) {
		timer.restart();
		unfinished++;

		VectorXZ pos = m_chunksToLoad.front();

		{ // finding the nearest chunk to load
			VectorXZ ppos = { toChunkPos(m_player->getX()), toChunkPos(m_player->getZ()) };
			float best_dist = distance(pos, ppos);
			for (auto it = m_chunksToLoad.begin(); it != m_chunksToLoad.end(); it++) {
				float dist = distance(*it, ppos);
				if (dist < best_dist) {
					best_dist = dist;
					pos = *it;
				}
			}
		}

		int x = pos.x;
		int z = pos.z;

		m_chunksMap.emplace(pos, Chunk(*this, pos));
		m_generator.generateTerrain(m_chunksMap[pos]);
		if (m_preChunksMap.find(pos) != m_preChunksMap.end()) {
			m_preChunksMap[pos].fillTo(m_chunksMap[pos]);
			m_preChunksMap.erase(pos);
		}

		addChunksToRegenMesh(pos);
		auto it = m_chunksToLoad.begin();
		while (it != m_chunksToLoad.end()) {
			if (*it == pos)
				m_chunksToLoad.erase(it++);
			else
				++it;
		}
	}

	/*if (unfinished > 2) {
		m_generator.finishGeneration(*this);
		unfinished = 0;
	}*/
}

void World::deleteChunks() {
 	for (auto &p : m_chunksToDelete)
		m_chunksMap.erase(p);

	m_chunksToDelete.clear();
}

void World::addChunksToRegenMesh(VectorXZ pos) {
	addChunkToRegenMesh(pos);
	addChunkToRegenMesh(pos + VectorXZ{ 1,  0 });
	addChunkToRegenMesh(pos + VectorXZ{ -1,  0 });
	addChunkToRegenMesh(pos + VectorXZ{ 0,  1 });
	addChunkToRegenMesh(pos + VectorXZ{ 0, -1 });
}

void World::addChunkToRegenMesh(VectorXZ pos) {
	m_isRegenChunks = true;
	if (m_chunksMap.find(pos) != m_chunksMap.end()) {
		m_chunksMap[pos].regenFlag = true;
	}
}

void World::addChunkToLoad(VectorXZ &pos) {
	m_chunksToLoad.push_front(pos);
}

void World::addChunkToDelete(VectorXZ &pos) {
	m_chunksToDelete.push_back(pos);
}