#include "Sim.h"
#include "AI.h"

#include <vector>
#include <cfloat>

// ---------------------------------------------------------------------------
// Tunable defaults. Everything the sim balances on lives here in one place.
// ---------------------------------------------------------------------------
Config Config_Default()
{
	Config c;

	c.fixedDt      = 1.0f / 60.0f;
	c.maxFrameTime = 0.25f;

	c.windowWidth  = 1600;
	c.windowHeight = 900;

	// Diagonal lane across the 16:9 frame: blue nexus lower-left, red upper-right,
	// pushed near the corners so the lane fills most of the screen.
	c.blueBase    = V(c.windowWidth * 0.09f, c.windowHeight * 0.85f);
	c.redBase     = V(c.windowWidth * 0.91f, c.windowHeight * 0.15f);
	c.laneWidth   = 450.f;
	c.blueTowerT  = 0.30f;
	c.redTowerT   = 0.70f;
	c.towerOffset = c.laneWidth * 0.22f; // push towers to one side of the lane axis
	c.champSpawnT = 0.15f;

	c.waveInterval      = 15.0f;
	c.spawnSpacing      = 0.65f; // time between spawns; also sets the column spacing
	c.meleePerWave      = 3;
	c.casterPerWave     = 3;
	c.cannonEveryNWaves = 3;

	// Separation keeps spacing; avoidance routes minions around blockers ahead so
	// waves flow past each other instead of shoving. The hard collision pass
	// (Phase_Collide) is the last-resort backstop against overlap.
	c.separationRange    = 42.0f;
	c.separationStrength = 26.0f;
	c.arriveRadius       = 60.0f;
	c.avoidLookahead     = 95.0f;
	c.avoidStrength      = 70.0f;

	c.detectRange     = 235.0f;
	c.targetLeash     = 60.0f;
	c.attackerMemory  = 3.0f;

	// hp, dmg, range, cooldown, speed, radius  (sizes/ranges scaled up to read big)
	c.meleeHp  = 120.0f; c.meleeDmg  = 12.0f; c.meleeRange  = 20.0f;  c.meleeCooldown  = 1.0f; c.meleeSpeed  = 80.0f; c.meleeRadius  = 16.0f;
	c.casterHp = 60.0f;  c.casterDmg = 18.0f; c.casterRange = 175.0f; c.casterCooldown = 1.5f; c.casterSpeed = 80.0f; c.casterRadius = 14.0f;
	c.cannonHp = 300.0f; c.cannonDmg = 40.0f; c.cannonRange = 200.0f; c.cannonCooldown = 2.5f; c.cannonSpeed = 72.0f; c.cannonRadius = 24.0f;

	c.towerHp = 1500.0f; c.towerDmg = 90.0f; c.towerRange = 270.0f; c.towerCooldown = 1.1f; c.towerRadius = 40.0f;

	c.champHp = 600.0f; c.champDmg = 55.0f; c.champRange = 200.0f; c.champCooldown = 0.6f; c.champSpeed = 130.0f; c.champRadius = 26.0f;

	c.projSpeed = 470.0f; c.projRadius = 9.0f;

	return c;
}

// ---------------------------------------------------------------------------
// Entity storage: index+generation handles over a reused vector.
// ---------------------------------------------------------------------------
Entity* World_Get(World& w, EntityId id)
{
	if (id.index == INVALID_INDEX || id.index >= w.ents.size())
		return nullptr;
	Entity& e = w.ents[id.index];
	if (!e.alive || e.id.gen != id.gen)
		return nullptr;
	return &e;
}

EntityId World_Spawn(World& w, const Entity& proto)
{
	if (!w.freeSlots.empty())
	{
		uint16_t idx = w.freeSlots.back();
		w.freeSlots.pop_back();
		uint16_t keepGen = w.ents[idx].id.gen; // generation was bumped on destroy
		w.ents[idx] = proto;
		w.ents[idx].id.index = idx;
		w.ents[idx].id.gen   = keepGen;
		w.ents[idx].alive    = true;
		return w.ents[idx].id;
	}

	Entity e = proto;
	e.id.index = (uint16_t)w.ents.size();
	e.id.gen   = 1;
	e.alive    = true;
	w.ents.push_back(e); // may reallocate; callers must not hold Entity& across this
	return e.id;
}

void World_Destroy(World& w, EntityId id)
{
	Entity* e = World_Get(w, id);
	if (!e)
		return;
	e->alive = false;
	e->id.gen++;               // invalidate every outstanding handle to this slot
	if (e->id.gen == 0)
		e->id.gen = 1;
	w.freeSlots.push_back(id.index);
}

// ---------------------------------------------------------------------------
// Queries used by the AI layer.
// ---------------------------------------------------------------------------
CP_Vector World_EnemyBasePoint(const World& w, Team team)
{
	if (team == TEAM_BLUE)
		return w.lane.redBase;
	return w.lane.blueBase;
}

// Nearest living enemy of a given kind within range. Ties break on the lower slot
// index so selection is deterministic regardless of iteration quirks.
Entity* World_NearestEnemy(World& w, const Entity& self, EntityKind kind, float maxRange, EntityId* outId)
{
	Entity* best = nullptr;
	float   bestD = maxRange;
	if (outId)
		*outId = InvalidId();

	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive || e.kind != kind || e.team == self.team)
			continue;
		float d = VDist(self.pos, e.pos);
		if (d < bestD || (best && d == bestD && e.id.index < best->id.index))
		{
			bestD = d;
			best  = &e;
			if (outId)
				*outId = e.id;
		}
	}
	return best;
}

// Pick the enemy body under a point (for right-clicking a target). Nearest wins so
// clicking an overlapping stack grabs the one you're closest to.
EntityId World_PickEnemyAt(World& w, CP_Vector p, Team myTeam, float slack)
{
	EntityId best = InvalidId();
	float    bestD = FLT_MAX;
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive || e.kind == KIND_PROJECTILE || e.team == myTeam)
			continue;
		float d = VDist(p, e.pos);
		if (d <= e.radius + slack && d < bestD)
		{
			bestD = d;
			best  = e.id;
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// Construction helpers.
// ---------------------------------------------------------------------------
static Entity MakeMinion(const Config& c, Team team, MinionType type, CP_Vector pos)
{
	Entity e = {};
	e.id           = InvalidId();
	e.kind         = KIND_MINION;
	e.team         = team;
	e.pos          = pos;
	e.vel          = CP_Vector_Zero();
	e.minionType   = type;
	e.state        = STATE_MARCHING;
	e.cooldownTimer= 0.0f;
	e.target       = InvalidId();
	e.lastAttacker = InvalidId();
	e.reactedAttacker = InvalidId();
	e.source       = InvalidId();
	e.projSpeed    = c.projSpeed;

	switch (type)
	{
	case MINION_MELEE:
		e.maxHp = c.meleeHp;  e.attackDamage = c.meleeDmg;  e.attackRange = c.meleeRange;
		e.attackCooldown = c.meleeCooldown; e.moveSpeed = c.meleeSpeed; e.radius = c.meleeRadius;
		break;
	case MINION_CASTER:
		e.maxHp = c.casterHp; e.attackDamage = c.casterDmg; e.attackRange = c.casterRange;
		e.attackCooldown = c.casterCooldown; e.moveSpeed = c.casterSpeed; e.radius = c.casterRadius;
		break;
	case MINION_CANNON:
		e.maxHp = c.cannonHp; e.attackDamage = c.cannonDmg; e.attackRange = c.cannonRange;
		e.attackCooldown = c.cannonCooldown; e.moveSpeed = c.cannonSpeed; e.radius = c.cannonRadius;
		break;
	}
	e.hp = e.maxHp;
	return e;
}

static Entity MakeTower(const Config& c, Team team, CP_Vector pos)
{
	Entity e = {};
	e.id = InvalidId();
	e.kind = KIND_TOWER;
	e.team = team;
	e.pos = pos;
	e.vel = CP_Vector_Zero();
	e.maxHp = c.towerHp; e.hp = c.towerHp;
	e.attackDamage = c.towerDmg;
	e.attackRange = c.towerRange;
	e.attackCooldown = c.towerCooldown;
	e.cooldownTimer = 0.0f;
	e.radius = c.towerRadius;
	e.target = InvalidId();
	e.lastAttacker = InvalidId();
	e.source = InvalidId();
	e.projSpeed = c.projSpeed;
	return e;
}

static Entity MakeChampion(const Config& c, Team team, CP_Vector pos)
{
	Entity e = {};
	e.id = InvalidId();
	e.kind = KIND_CHAMPION;
	e.team = team;
	e.pos = pos;
	e.vel = CP_Vector_Zero();
	e.maxHp = c.champHp; e.hp = c.champHp;
	e.attackDamage = c.champDmg;
	e.attackRange = c.champRange;
	e.attackCooldown = c.champCooldown;
	e.cooldownTimer = 0.0f;
	e.moveSpeed = c.champSpeed;
	e.radius = c.champRadius;
	e.state = STATE_MARCHING;
	e.target = InvalidId();
	e.lastAttacker = InvalidId();
	e.source = InvalidId();
	e.projSpeed = c.projSpeed;
	return e;
}

// ---------------------------------------------------------------------------
// World lifecycle.
// ---------------------------------------------------------------------------
void World_Init(World& w, uint32_t seed)
{
	w.cfg = Config_Default();
	Rng_Seed(w.rng, seed);
	w.tick = 0;
	w.ents.clear();
	w.freeSlots.clear();
	w.blueKills = 0;
	w.redKills  = 0;

	w.lane.blueBase = w.cfg.blueBase;
	w.lane.redBase  = w.cfg.redBase;
	w.lane.width    = w.cfg.laneWidth;

	// Towers sit off the lane axis so minions can march straight down the centre.
	// Blue offsets up (toward -y), red offsets down (+y), so they flank opposite sides.
	CP_Vector dir  = Lane_Dir(w.lane);
	CP_Vector perp = V(-dir.y, dir.x);
	if (perp.y < 0.0f) perp = VScale(perp, -1.0f); // make perp point "down" (+y)
	CP_Vector blueOff = VScale(perp, -w.cfg.towerOffset); // up
	CP_Vector redOff  = VScale(perp,  w.cfg.towerOffset); // down
	w.blueTower = World_Spawn(w, MakeTower(w.cfg, TEAM_BLUE, VAdd(Lane_PointAt(w.lane, w.cfg.blueTowerT), blueOff)));
	w.redTower  = World_Spawn(w, MakeTower(w.cfg, TEAM_RED,  VAdd(Lane_PointAt(w.lane, w.cfg.redTowerT), redOff)));
	w.champion  = World_Spawn(w, MakeChampion(w.cfg, TEAM_BLUE, Lane_PointAt(w.lane, w.cfg.champSpawnT)));

	for (int t = 0; t < 2; ++t)
	{
		w.waves[t].team          = (Team)t;
		w.waves[t].number        = 0;
		w.waves[t].nextWaveTimer = 1.0f; // first wave kicks off almost immediately
		w.waves[t].spawnTimer    = 0.0f;
		w.waves[t].queue.clear();
	}

	// Champion starts idle (no order).
	w.champOrder   = InvalidId();
	w.champGoal    = CP_Vector_Zero();
	w.champHasGoal = false;
	w.champPath.clear();
	w.champPathIdx = 0;

	// Build the navigation grid + flow fields now that the towers are in place.
	Nav_Build(w, w.nav);
}

// ---------------------------------------------------------------------------
// Phase 1: spawn.
// ---------------------------------------------------------------------------
static void Phase_Spawn(World& w, float dt)
{
	for (int t = 0; t < 2; ++t)
	{
		Wave& wv = w.waves[t];
		Team team = (Team)t;
		wv.nextWaveTimer -= dt;

		if (wv.queue.empty() && wv.nextWaveTimer <= 0.0f)
		{
			wv.number++;
			for (int i = 0; i < w.cfg.meleePerWave; ++i)  wv.queue.push_back(MINION_MELEE);
			for (int i = 0; i < w.cfg.casterPerWave; ++i) wv.queue.push_back(MINION_CASTER);
			if (w.cfg.cannonEveryNWaves > 0 && (wv.number % w.cfg.cannonEveryNWaves) == 0)
				wv.queue.push_back(MINION_CANNON);
			wv.nextWaveTimer = w.cfg.waveInterval;
			wv.spawnTimer    = 0.0f; // first minion this wave spawns now
		}

		if (!wv.queue.empty())
		{
			wv.spawnTimer -= dt;
			if (wv.spawnTimer <= 0.0f)
			{
				MinionType type = wv.queue.front();
				wv.queue.erase(wv.queue.begin());

				// Spawn on the lane axis just outside the nexus. Because spawns are
				// time-staggered, the wave forms a single-file column down the lane.
				float spawnT = (team == TEAM_BLUE) ? 0.03f : 0.97f;
				World_Spawn(w, MakeMinion(w.cfg, team, type, Lane_PointAt(w.lane, spawnT)));

				wv.spawnTimer = w.cfg.spawnSpacing;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Phase 2: sense. Advance per-entity timers; forget stale attackers.
// ---------------------------------------------------------------------------
static void Phase_Sense(World& w, float dt)
{
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive)
			continue;
		if (e.cooldownTimer > 0.0f)       e.cooldownTimer -= dt;
		if (e.championTriggerTimer > 0.0f) e.championTriggerTimer -= dt;
		if (e.lastAttackerTimer > 0.0f)
		{
			e.lastAttackerTimer -= dt;
			if (e.lastAttackerTimer <= 0.0f)
				e.lastAttacker = InvalidId();
		}
	}
}

// ---------------------------------------------------------------------------
// Phase 3: decide. Dispatch to the AI layer per kind.
// ---------------------------------------------------------------------------
static void Phase_Decide(World& w, const SimInput& input)
{
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive)
			continue;
		switch (e.kind)
		{
		case KIND_MINION:   AI_DecideMinion(w, e); break;
		case KIND_TOWER:    AI_DecideTower(w, e); break;
		case KIND_CHAMPION: AI_DecideChampion(w, e, input); break;
		default: break; // projectiles are handled in resolve
		}
	}
}

// A projectile we decided to fire this tick, spawned only after the act loop so a
// vector reallocation can't invalidate the references act is iterating over.
struct PendingShot
{
	EntityId  source;
	EntityId  target;
	CP_Vector pos;
	float     dmg;
	Team      team;
	float     speed;
};

static void Damage(World& w, Entity& tgt, float dmg, EntityId src)
{
	tgt.hp -= dmg;
	tgt.lastAttacker      = src;
	tgt.lastAttackerTimer = w.cfg.attackerMemory;
}

// ---------------------------------------------------------------------------
// Phase 4: act. Integrate movement and start attacks.
// ---------------------------------------------------------------------------
static void Phase_Act(World& w, float dt, std::vector<PendingShot>& pending)
{
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive || e.kind == KIND_PROJECTILE)
			continue;

		// Movement (towers are immobile). Overlap and lane-band clamping are handled
		// afterwards in Phase_Collide.
		if (e.kind == KIND_MINION || e.kind == KIND_CHAMPION)
			e.pos = VAdd(e.pos, VScale(e.vel, dt));

		// Attack when off cooldown and a target is in range.
		if (e.cooldownTimer <= 0.0f && IsValidId(e.target))
		{
			Entity* tgt = World_Get(w, e.target);
			if (tgt && VDist(e.pos, tgt->pos) <= e.attackRange + tgt->radius)
			{
				bool melee = (e.kind == KIND_MINION && e.minionType == MINION_MELEE);
				if (melee)
				{
					Damage(w, *tgt, e.attackDamage, e.id); // instant, no travel time
				}
				else
				{
					PendingShot s;
					s.source = e.id; s.target = e.target; s.pos = e.pos;
					s.dmg = e.attackDamage; s.team = e.team; s.speed = e.projSpeed;
					pending.push_back(s);
				}
				e.cooldownTimer = e.attackCooldown;
			}
		}
	}

	// Now it is safe to grow the entity vector.
	for (size_t i = 0; i < pending.size(); ++i)
	{
		const PendingShot& s = pending[i];
		Entity p = {};
		p.id = InvalidId();
		p.kind = KIND_PROJECTILE;
		p.team = s.team;
		p.pos = s.pos;
		p.radius = w.cfg.projRadius;
		p.attackDamage = s.dmg;
		p.source = s.source;
		p.target = s.target;
		p.projSpeed = s.speed;
		World_Spawn(w, p);
	}
	pending.clear();
}

// ---------------------------------------------------------------------------
// Phase 5: resolve. Move projectiles, apply damage, then sweep the dead.
// ---------------------------------------------------------------------------
static void Phase_Resolve(World& w, float dt)
{
	// Committed projectiles home on their target; if the target is gone they miss.
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& p = w.ents[i];
		if (!p.alive || p.kind != KIND_PROJECTILE)
			continue;

		Entity* tgt = World_Get(w, p.target);
		if (!tgt)
		{
			World_Destroy(w, p.id);
			continue;
		}

		CP_Vector to = VSub(tgt->pos, p.pos);
		float d = VLen(to);
		float step = p.projSpeed * dt;
		if (d <= step + tgt->radius + p.radius)
		{
			Damage(w, *tgt, p.attackDamage, p.source);
			World_Destroy(w, p.id);
		}
		else
		{
			p.pos = VAdd(p.pos, VScale(VNorm(to), step));
		}
	}

	// Death sweep. Champions respawn at base instead of being destroyed.
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive || e.kind == KIND_PROJECTILE)
			continue;
		if (e.hp > 0.0f)
			continue;

		if (e.kind == KIND_CHAMPION)
		{
			e.hp = e.maxHp;
			e.pos = Lane_PointAt(w.lane, w.cfg.champSpawnT);
			e.target = InvalidId();
			e.cooldownTimer = 1.0f;
			continue;
		}
		if (e.kind == KIND_MINION)
		{
			if (e.team == TEAM_RED) w.blueKills++;
			else                    w.redKills++;
		}
		World_Destroy(w, e.id);
	}
}

// ---------------------------------------------------------------------------
// Phase 4b: collide. Push overlapping bodies apart so nothing passes through
// anything else. Inverse mass sets who yields: towers never move, the champion is
// heavy (minions can body-block it but not shove it around), cannons are sturdy.
// ---------------------------------------------------------------------------
static float InvMass(const Entity& e)
{
	if (e.kind == KIND_TOWER)    return 0.0f;
	if (e.kind == KIND_CHAMPION) return 0.25f;
	if (e.kind == KIND_MINION && e.minionType == MINION_CANNON) return 0.5f;
	return 1.0f;
}

static void Phase_Collide(World& w)
{
	const int iterations = 3; // a few relaxation passes keep dense stacks stable

	for (int it = 0; it < iterations; ++it)
	{
		for (size_t i = 0; i < w.ents.size(); ++i)
		{
			Entity& a = w.ents[i];
			if (!a.alive || a.kind == KIND_PROJECTILE)
				continue;

			for (size_t j = i + 1; j < w.ents.size(); ++j)
			{
				Entity& b = w.ents[j];
				if (!b.alive || b.kind == KIND_PROJECTILE)
					continue;

				float wi = InvMass(a);
				float wj = InvMass(b);
				float wsum = wi + wj;
				if (wsum <= 0.0f)
					continue; // both immovable

				CP_Vector d = VSub(b.pos, a.pos);
				float dist = VLen(d);
				float rr = a.radius + b.radius;
				if (dist >= rr)
					continue;

				float overlap = rr - dist;
				CP_Vector n;
				if (dist > 0.0001f)
					n = VScale(d, 1.0f / dist);
				else
				{
					// Exactly coincident: shove apart across the lane, deterministically.
					CP_Vector dir = Lane_Dir(w.lane);
					n = V(-dir.y, dir.x);
				}

				a.pos = VSub(a.pos, VScale(n, overlap * (wi / wsum)));
				b.pos = VAdd(b.pos, VScale(n, overlap * (wj / wsum)));
			}
		}
	}

	// Keep everything that can move inside the lane band.
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive || e.kind == KIND_PROJECTILE || e.kind == KIND_TOWER)
			continue;
		e.pos = Lane_Clamp(w.lane, e.pos, e.radius);
	}
}

// ---------------------------------------------------------------------------
// One fixed simulation step.
// ---------------------------------------------------------------------------
void Sim_Tick(World& w, float dt, const SimInput& input)
{
	static std::vector<PendingShot> pending; // reused scratch; sim is single-threaded

	Phase_Spawn(w, dt);
	Phase_Sense(w, dt);
	Phase_Decide(w, input);
	Phase_Act(w, dt, pending);
	Phase_Collide(w);
	Phase_Resolve(w, dt);

	w.tick++;
}
