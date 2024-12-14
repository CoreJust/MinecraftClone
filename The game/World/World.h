#pragma once
#include <unordered_map>
#include <list>
#include "../Maths/VectorXZ.h"
#include "../Maths/BlockPos.h"
#include "../Utils/Random.h"
#include "ItemDatabase.h"
#include "../Renderer/RenderMaster.h"
#include "../Entity/Player.h"
#include "Chunk/Chunk.h"
#include "Chunk\PreChunk.h"
#include "Generator\TerrainGenerator\BasicGenerator.h"

constexpr int DAY_SIZE = 180;
constexpr float DAY_START = DAY_SIZE * 0.08f;
constexpr float DAY_END = DAY_SIZE * 0.42;
constexpr float NIGHT_START = DAY_SIZE * 0.62;
constexpr float NIGHT_END = DAY_SIZE * 0.88;

class World final {
public:
	void generate(int pX, int pZ);

	void update(glm::vec3 &camPos, float dt);
	void updateEntities(float dt);
	void tick();
	int draw(RenderMaster &renderer, Camera &cam);

	std::unordered_map<VectorXZ, Chunk>& getChunks();
	IChunk* getChunk(VectorXZ pos);

	bool isOnGround(glm::vec3 &pos, const glm::vec3 &box); // is entity on position pos on ground
	bool isInLiquid(glm::vec3 &pos, const glm::vec3 &box);

	void destroyBlock(const BlockPos &pos);
	bool trySetBlock(ItemID block, const BlockPos &pos, Player &player); // try to set block by player if it possible
	void setBlock(ItemID block, const BlockPos &pos); // just set block
	void addUpdatablesFor(const BlockPos &pos);

	void fillBlocks(ItemID block, int x, int sizeX, int y, int sizeY, int z, int sizeZ); // fill an area by blocks
	void destroyBlocks(int x, int sizeX, int y, int sizeY, int z, int sizeZ); // destroy an area

	ItemID getBlock(int x, int y, int z) const;
	ItemID getBlock(const BlockPos &pos) const;
	bool hasNeighbours(Chunk &chunk, const BlockPos &pos) const;

	void summon(Entity *entity);

	void setPlayer(Player *p);
	Player* getPlayer();

	void setRD(int rd);
	int getRD() const;

	int getWorldDay() const;
	float getWorldTime() const;

	bool isDay() const;
	bool isNight() const;
	bool isMorning() const;
	bool isEvening() const;

	const Random<std::mt19937>& getRandom() const;

private:
	void loadChunks();
	void deleteChunks();

	void addChunksToRegenMesh(VectorXZ pos);
	void addChunkToRegenMesh(VectorXZ pos);
	void addChunkToLoad(VectorXZ &pos);
	void addChunkToDelete(VectorXZ &pos);

private:
	Player *m_player;
	std::vector<Entity*> m_entities;

	BasicGenerator m_generator;
	std::unordered_map<VectorXZ, Chunk> m_chunksMap;
	std::unordered_map<VectorXZ, PreChunk> m_preChunksMap;

	std::list<VectorXZ> m_chunksToLoad;
	std::vector<VectorXZ> m_chunksToDelete;
	std::unordered_map<BlockPos, float> m_updatables;

	int m_RD = 7;
	int m_spawnDistance = 64;
	float m_worldTime = 0.f;
	bool m_isRegenChunks = false;
};