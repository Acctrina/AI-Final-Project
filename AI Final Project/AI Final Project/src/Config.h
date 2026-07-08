#pragma once

// Core types shared across the whole simulation: enums, entity handles, a small
// vector-math shim over CP_Vector, a deterministic RNG, and the tunables struct.
// This header carries no rendering or UI code so the sim stays inspectable.

#include <cstdint>
#include "cprocessing.h"

// --- Teams and entity taxonomy -------------------------------------------------

enum Team
{
	TEAM_BLUE,
	TEAM_RED,
	TEAM_NEUTRAL
};

enum EntityKind
{
	KIND_MINION,
	KIND_TOWER,
	KIND_CHAMPION,
	KIND_PROJECTILE
};

enum MinionType
{
	MINION_MELEE,
	MINION_CASTER,
	MINION_CANNON
};

// Intentionally three states; every behaviour below has to fall out of these.
enum FsmState
{
	STATE_MARCHING,
	STATE_IN_COMBAT,
	STATE_THREATENED
};

// How the enemy (red) champion is driven. The player champion is always mouse-driven;
// this only selects the brain for the AI-controlled side. Selectable at runtime so the
// same build can show a lane bully, a passive target dummy, or a zone defender.
enum EnemyChampMode
{
	ENEMY_CHAMP_LANE_PUSHER, // marches with its wave, engages, retreats when low
	ENEMY_CHAMP_DUMMY,       // stands near its tower, only swings at whatever is in range
	ENEMY_CHAMP_HOLDER       // defends a bubble around its spawn, returns when the zone clears
};

// --- Entity handles ------------------------------------------------------------
// Minions die constantly and storage is a growing/reused vector, so we never keep
// raw pointers between ticks. A handle is a slot index plus a generation counter;
// when a slot is reused the generation bumps and every stale handle resolves null.

static const uint16_t INVALID_INDEX = 0xFFFF;

struct EntityId
{
	uint16_t index;
	uint16_t gen;
};

inline EntityId InvalidId()          { EntityId id; id.index = INVALID_INDEX; id.gen = 0; return id; }
inline bool     IsValidId(EntityId a){ return a.index != INVALID_INDEX; }
inline bool     SameId(EntityId a, EntityId b) { return a.index == b.index && a.gen == b.gen; }

// --- Vector-math shim ----------------------------------------------------------
// Thin wrappers so the sim reads cleanly. CP_Vector is plain {x,y} data; using the
// framework's math here is pure (deterministic) and keeps us from reinventing it.

inline CP_Vector V(float x, float y)              { return CP_Vector_Set(x, y); }
inline CP_Vector VAdd(CP_Vector a, CP_Vector b)   { return CP_Vector_Add(a, b); }
inline CP_Vector VSub(CP_Vector a, CP_Vector b)   { return CP_Vector_Subtract(a, b); }
inline CP_Vector VScale(CP_Vector a, float s)     { return CP_Vector_Scale(a, s); }
inline float     VLen(CP_Vector a)                { return CP_Vector_Length(a); }
inline float     VDist(CP_Vector a, CP_Vector b)  { return CP_Vector_Distance(a, b); }

// Normalize that is safe on the zero vector (returns zero instead of NaN).
inline CP_Vector VNorm(CP_Vector a)
{
	float l = VLen(a);
	if (l > 0.0001f)
		return VScale(a, 1.0f / l);
	return CP_Vector_Zero();
}

// Clamp a vector to a maximum length.
inline CP_Vector VLimit(CP_Vector a, float maxLen)
{
	float l = VLen(a);
	if (l > maxLen && l > 0.0001f)
		return VScale(a, maxLen / l);
	return a;
}

// --- Deterministic RNG ---------------------------------------------------------
// Our own seedable xorshift, kept per-World, so scenarios replay identically. We
// deliberately do NOT use CProcessing's global RNG (shared mutable state).

struct Rng
{
	uint32_t s;
};

inline void Rng_Seed(Rng& r, uint32_t seed) { r.s = seed ? seed : 0x9E3779B9u; }

inline uint32_t Rng_Next(Rng& r)
{
	r.s ^= r.s << 13;
	r.s ^= r.s >> 17;
	r.s ^= r.s << 5;
	return r.s;
}

inline float Rng_Float(Rng& r) // [0, 1)
{
	return (Rng_Next(r) & 0xFFFFFFu) / (float)0x1000000;
}

inline float Rng_Range(Rng& r, float lo, float hi)
{
	return lo + Rng_Float(r) * (hi - lo);
}

// --- Tunables ------------------------------------------------------------------
// Every magic number lives here so tuning (and later, per-scenario overrides) has
// exactly one home. Defaults are filled by Config_Default().

struct Config
{
	// Timing
	float fixedDt;        // sim step length; sim never reads wall-clock
	float maxFrameTime;   // clamp on accumulated real time (spiral-of-death guard)

	// Window size. Single knob for resolution; the lane is placed relative to it.
	int   windowWidth;
	int   windowHeight;

	// The lane runs diagonally between two base points (League/Dota-style) with a
	// fixed band width. Towers and the champion spawn sit at parameters along it.
	CP_Vector blueBase;    // lower-left nexus
	CP_Vector redBase;     // upper-right nexus
	float     laneWidth;
	float     blueTowerT;  // tower position along the lane: 0 at blue base .. 1 at red
	float     redTowerT;
	float     towerOffset; // perpendicular shift off the lane axis, so the centre stays clear
	float     champSpawnT;
	float     champSpawnOffset; // perpendicular shift for champion spawns, so they don't
	                            // sit on the axis and block their own wave at the start

	// Waves
	float waveInterval;      // seconds between waves
	float spawnSpacing;      // seconds between minions within a wave
	int   meleePerWave;
	int   casterPerWave;
	int   cannonEveryNWaves; // a siege/cannon minion joins every Nth wave

	// Steering
	float separationRange;
	float separationStrength;
	float arriveRadius;
	float avoidLookahead;   // how far ahead a minion looks for blockers to route around
	float avoidStrength;    // lateral force applied to steer around a blocker

	// Minion stats: hp, damage, attackRange, cooldown, speed, radius
	float meleeHp,   meleeDmg,   meleeRange,   meleeCooldown,   meleeSpeed,   meleeRadius;
	float casterHp,  casterDmg,  casterRange,  casterCooldown,  casterSpeed,  casterRadius;
	float cannonHp,  cannonDmg,  cannonRange,  cannonCooldown,  cannonSpeed,  cannonRadius;

	// Minion target acquisition radius
	float detectRange;
	// Extra range past detectRange before a minion drops a target it already committed
	// to (hysteresis, so it holds a target instead of re-picking "closest" every tick).
	float targetLeash;
	// How long a minion stays aggro'd on an enemy champion after that champion's last
	// hit. Only champion hits refresh it, so the minion returns to the enemy wave this
	// many seconds after you stop attacking it (feeds the "threatened" rule).
	float championAggroTime;
	// Grace window after a champion hit during which its aggro is protected. An enemy
	// minion hitting the same minion can only steal aggro back to the wave once this long
	// has passed since the champion's last hit - so continuous attacking (a dive) holds
	// aggro, but the moment you disengage a minion attacker reclaims the target. Keep it
	// >= the champion's attack cooldown.
	float championAggroHold;

	// Tower
	float towerHp, towerDmg, towerRange, towerCooldown, towerRadius;
	// Tower-aggro manipulation: attacking an enemy champion inside its tower's range
	// makes that tower drop the minion it was hitting and lock onto you for this long
	// (refreshed by each hit). Disengage or leave range and it reverts to minions.
	float towerChampAggroTime;

	// Champion (player)
	float champHp, champDmg, champRange, champCooldown, champSpeed, champRadius;

	// Enemy (red) champion. Shares the champion stat block above; these only govern
	// where it lives and how its AI decides.
	int   enemyChampMode;          // one of EnemyChampMode (integer knob, tweakable live)
	float redChampSpawnT;          // spawn/anchor position along the lane (near red base)
	float enemyChampRetreatHpFrac; // lane-pusher falls back to its tower below this hp fraction
	// "Defend your champion" aggro: when a champion damages an enemy champion, that
	// champion's allied minions within this radius switch their aggro onto the attacker
	// (League minion rule #1). They release on the normal championAggroTime decay, so
	// they drift back to the wave once you stop hitting their champion.
	float defendRadius;

	// Projectiles (committed, homing shots for ranged attackers)
	float projSpeed, projRadius;

	// --- Analysis / emergence layer --------------------------------------------
	// A read-only observer samples unit presence into columns along the lane, finds
	// the wave-equilibrium point (where the two sides balance), and names emergent
	// techniques from how that point moves. None of these values affect the sim.
	int   influenceCols;      // lane sampling resolution (columns from blue to red base)
	float influenceSpread;    // splat kernel half-width, in lane fraction (0..1)
	float influenceMinionW;   // per-unit presence weights
	float influenceChampW;
	float influenceTowerW;

	float equilVelSmoothing;  // low-pass factor (0..1) on the equilibrium velocity
	float freezeVelEps;       // |equil vel| below this counts as "held" (lane frac/sec)
	float freezeHoldTime;     // seconds the front must stay held before "freeze" fires
	float slowPushVel;        // |vel| up to here (but above freeze) reads as a slow push
	float fastPushVel;        // |vel| at/above here reads as a fast push / shove
	float pushHoldTime;       // seconds a push must persist before it is announced

	float blockRadius;        // champion-to-enemy-minion distance counted as a block
	int   blockMinCount;      // enemy minions stalled by the champion to call it a block

	float bannerTtl;          // seconds a recognised-technique banner stays on screen
	float techCooldown;       // per-technique re-fire cooldown so banners don't spam
};

Config Config_Default();
