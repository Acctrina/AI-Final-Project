#pragma once

// The simulation core: a single flat Entity struct (tagged by kind), the World that
// owns them, and the fixed-step tick. No draw calls live here - rendering and UI
// read this state, they never sit inside it.

#include <vector>
#include "Config.h"
#include "Nav.h"

// One flat struct for every kind of thing in the lane. Fields not relevant to a given
// kind simply go unused. Behaviour is decided by free functions, not methods.
struct Entity
{
	EntityId   id;
	EntityKind kind;
	Team       team;
	bool       alive;

	CP_Vector  pos;
	CP_Vector  vel;
	float      radius;

	float      hp;
	float      maxHp;

	// Combat
	float      attackDamage;
	float      attackRange;
	float      attackCooldown; // seconds between attacks
	float      cooldownTimer;  // counts down to 0, then may attack again
	EntityId   target;

	// Minion
	MinionType minionType;
	FsmState   state;
	float      moveSpeed;
	EntityId   champAggressor;  // enemy champion that drew this minion's aggro, else invalid
	float      champAggroTimer; // seconds of champion-aggro left; refreshed only by a champion
	                            // hit, so ordinary minion combat can't keep the lock alive

	// Tower
	float      championTriggerTimer; // >0 while locked onto a champion that attacked an
	                                 // allied champion in range (tower-aggro manipulation)

	// Projectile
	EntityId   source;
	float      projSpeed;
};

// A lane is the geometry everything lives on: a straight segment between the two bases
// plus a band width. All along-lane / cross-lane math goes through these helpers.
struct Lane
{
	CP_Vector blueBase;
	CP_Vector redBase;
	float     width;
};

inline CP_Vector Lane_Dir(const Lane& lane)
{
	return VNorm(VSub(lane.redBase, lane.blueBase));
}

inline CP_Vector Lane_PointAt(const Lane& lane, float t)
{
	return VAdd(lane.blueBase, VScale(VSub(lane.redBase, lane.blueBase), t));
}

// Projection onto the lane axis: 0 at blue base .. 1 at red base.
inline float Lane_Coord(const Lane& lane, CP_Vector pos)
{
	CP_Vector d = VSub(lane.redBase, lane.blueBase);
	float dd = d.x * d.x + d.y * d.y;
	if (dd < 0.0001f) return 0.0f;
	CP_Vector rel = VSub(pos, lane.blueBase);
	return (rel.x * d.x + rel.y * d.y) / dd;
}

// Keep a point inside the lane band by clamping its perpendicular distance to the
// axis (movement along the lane is unrestricted, so minions still reach the base).
inline CP_Vector Lane_Clamp(const Lane& lane, CP_Vector pos, float radius)
{
	CP_Vector dir    = Lane_Dir(lane);
	CP_Vector rel    = VSub(pos, lane.blueBase);
	float     along  = rel.x * dir.x + rel.y * dir.y;
	CP_Vector onLine = VAdd(lane.blueBase, VScale(dir, along));
	CP_Vector perp   = VSub(pos, onLine);
	float     pd     = VLen(perp);
	float     maxPerp = lane.width * 0.5f - radius;
	if (maxPerp < 0.0f) maxPerp = 0.0f;
	if (pd > maxPerp && pd > 0.0001f)
		return VAdd(onLine, VScale(perp, maxPerp / pd));
	return pos;
}

// A wave is a spawner plus a logical grouping, not a hard container. It holds the
// queue of minions still to trickle out, and the countdown to the next wave.
struct Wave
{
	Team                    team;
	int                     number;
	float                   nextWaveTimer;  // until the next wave is queued
	float                   spawnTimer;      // spacing between individual spawns
	std::vector<MinionType> queue;           // minions left to spawn this wave
};

// A single mouse order for the champion this tick (MOBA-style right-click).
struct SimInput
{
	bool      issued;       // was an order given this tick?
	CP_Vector worldPoint;   // cursor location (world == screen here)
	EntityId  targetEntity; // enemy under the cursor, else invalid
};

struct World
{
	Config    cfg;
	Rng       rng;
	long long tick;

	std::vector<Entity>   ents;
	std::vector<uint16_t> freeSlots; // reusable entity slots

	Lane      lane;
	NavGrid   nav;       // static navigation grid + flow fields
	Wave      waves[2];  // indexed by Team (blue, red)

	EntityId  blueTower;
	EntityId  redTower;
	EntityId  champion;      // blue-side player
	EntityId  enemyChampion; // red-side, AI-driven (see EnemyChampMode)

	// Champion mouse-command state (persists between orders).
	EntityId               champOrder;    // enemy to attack-move to, else invalid
	CP_Vector              champGoal;     // ground destination for a move order
	bool                   champHasGoal;
	std::vector<CP_Vector> champPath;     // A* waypoints toward champGoal
	size_t                 champPathIdx;

	// Simple counters surfaced in the HUD.
	int       blueKills;
	int       redKills;
};

// --- Entity management ---------------------------------------------------------

Entity*  World_Get(World& w, EntityId id);      // null if stale/dead
EntityId World_Spawn(World& w, const Entity& proto);
void     World_Destroy(World& w, EntityId id);

// --- Lifecycle -----------------------------------------------------------------

void World_Init(World& w, uint32_t seed);
void Sim_Tick(World& w, float dt, const SimInput& input);

// Helpers used by the AI layer and renderer.
CP_Vector World_EnemyBasePoint(const World& w, Team team);
Entity*   World_NearestEnemy(World& w, const Entity& self, EntityKind kind, float maxRange, EntityId* outId);
// Topmost living enemy of myTeam whose body is under point p (within slack), else invalid.
EntityId  World_PickEnemyAt(World& w, CP_Vector p, Team myTeam, float slack);
