#include "AI.h"

// ---------------------------------------------------------------------------
// Steering primitives. Each returns a velocity contribution; callers sum and
// clamp them. Nothing here mutates the world.
// ---------------------------------------------------------------------------

// Seek with arrival: full speed far away, easing to a stop inside arriveRadius,
// and zero once within stopRadius (e.g. already in attack range of the target).
static CP_Vector Steer_Arrive(CP_Vector pos, CP_Vector goal, float maxSpeed,
                              float stopRadius, float arriveRadius)
{
	CP_Vector to = VSub(goal, pos);
	float d = VLen(to);
	if (d <= stopRadius)
		return CP_Vector_Zero();

	float speed = maxSpeed;
	if (d < arriveRadius)
		speed = maxSpeed * (d / arriveRadius);
	return VScale(VNorm(to), speed);
}

// Push away from nearby minions so the wave spreads instead of stacking on a point.
static CP_Vector Steer_Separation(World& w, const Entity& self, float range, float strength)
{
	CP_Vector acc = CP_Vector_Zero();
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		const Entity& o = w.ents[i];
		if (!o.alive || o.kind != KIND_MINION || o.id.index == self.id.index)
			continue;
		CP_Vector away = VSub(self.pos, o.pos);
		float d = VLen(away);
		if (d > 0.0001f && d < range)
			acc = VAdd(acc, VScale(VNorm(away), (range - d) / range));
	}
	return VScale(acc, strength);
}

// Route around whatever is directly ahead instead of shoving through it: any unit
// (friend or foe) except the one we're actually trying to reach. Also reports a
// brake factor (0..1) so the caller can slow down when a blocker is close and
// head-on, letting the minion slip around rather than ram. This is what makes waves
// flow past each other like a real MOBA lane instead of a jittering pile.
static CP_Vector Steer_Avoid(World& w, const Entity& self, EntityId targetId,
                             CP_Vector dir, float lookahead, float strength, float& brake)
{
	brake = 1.0f;
	if (VLen(dir) < 0.0001f)
		return CP_Vector_Zero();

	CP_Vector perp = V(-dir.y, dir.x);
	bool  found    = false;
	float bestFwd  = lookahead;
	float bestSide = 0.0f;
	float bestPush = 0.0f;

	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		const Entity& o = w.ents[i];
		if (!o.alive || o.kind == KIND_PROJECTILE || o.id.index == self.id.index)
			continue;
		if (SameId(o.id, targetId))
			continue; // never dodge the unit we're trying to reach

		// A teammate leading in roughly the same direction is a column to follow, not
		// an obstacle to pass - skip it so minions stay single-file instead of fanning
		// out. Only stopped or cross-moving teammates get routed around.
		if (o.team == self.team && o.kind == KIND_MINION)
		{
			float ospeed = VLen(o.vel);
			if (ospeed > 1.0f && (o.vel.x * dir.x + o.vel.y * dir.y) / ospeed > 0.6f)
				continue;
		}

		CP_Vector to  = VSub(o.pos, self.pos);
		float     fwd = to.x * dir.x + to.y * dir.y; // distance ahead along travel dir
		if (fwd <= 0.0f || fwd > lookahead)
			continue;                                 // behind me, or too far to matter

		float side  = to.x * perp.x + to.y * perp.y;  // signed lateral offset from my path
		float aside = side < 0.0f ? -side : side;
		float clearance = self.radius + o.radius + 8.0f;
		if (aside >= clearance)
			continue;                                 // not actually in my path

		if (fwd < bestFwd)                            // steer around the nearest blocker
		{
			found    = true;
			bestFwd  = fwd;
			bestSide = side;
			bestPush = (clearance - aside) / clearance;
		}
	}

	if (!found)
		return CP_Vector_Zero();

	float urgency = 1.0f - bestFwd / lookahead;

	// Slow down when a blocker is close and head-on so we ease around it, not into it.
	brake = 1.0f - 0.75f * bestPush * urgency;

	// Steer to the opposite side of the blocker; closer + more head-on => stronger.
	float sign = (bestSide > 0.0f) ? -1.0f : 1.0f;
	return VScale(perp, sign * strength * urgency * (0.5f + 0.5f * bestPush));
}

// ---------------------------------------------------------------------------
// Minion FSM.
//
// The three states are a genuine controller, not a label: `m.state` is
// authoritative and persists between ticks. Each tick we first run the
// transitions leaving the current state (which may hand the minion a new
// target), then execute the behaviour that the resulting state prescribes.
// Every distance check below belongs to exactly one transition, so it is the
// STATE - not a fresh "closest enemy" scan every frame - that decides what a
// minion does.
//
//   MARCHING   no target; walk down the lane toward the enemy base.
//   IN_COMBAT  committed to an enemy (minion > champion > tower); close & fight.
//   THREATENED retaliating against an enemy champion that just hit us.
//
// Transitions:
//   any state --(new enemy-champion attacker)--> THREATENED
//   MARCHING  --(enemy in detect range)-------->  IN_COMBAT
//   IN_COMBAT --(target lost, another near)---->  IN_COMBAT (re-acquire)
//   IN_COMBAT --(target lost, lane clear)------>  MARCHING
//   THREATENED--(champion gone, enemy near)---->  IN_COMBAT
//   THREATENED--(champion gone, lane clear)---->  MARCHING
// ---------------------------------------------------------------------------

// Fixed aggro priority: the closest enemy minion, then champion, then tower,
// all within detection range. Returns null (tgtId invalid) if the lane is clear.
static Entity* Minion_Acquire(World& w, Entity& m, EntityId& tgtId)
{
	const Config& cfg = w.cfg;
	Entity* t = World_NearestEnemy(w, m, KIND_MINION, cfg.detectRange, &tgtId);
	if (!t) t = World_NearestEnemy(w, m, KIND_CHAMPION, cfg.detectRange, &tgtId);
	if (!t) t = World_NearestEnemy(w, m, KIND_TOWER,   cfg.detectRange, &tgtId);
	return t;
}

// A *new* enemy-champion attacker we have not answered yet. Only champions
// trigger retaliation - enemy minion pokes (a caster outranging our melee) must
// not pull the wave off its front line - and each attacker fires the transition
// once, so continuous fire doesn't thrash the target. Null if no fresh threat.
static Entity* Minion_NewThreat(World& w, Entity& m)
{
	Entity* atk = World_Get(w, m.lastAttacker);
	if (atk && atk->team != m.team && atk->kind == KIND_CHAMPION &&
	    m.lastAttackerTimer > 0.0f && !SameId(m.lastAttacker, m.reactedAttacker))
		return atk;
	return nullptr;
}

// Is a held target still worth committing to? Alive, hostile, and inside
// detect range + leash - the hysteresis that stops minions thrashing between
// targets in a scrum.
static bool Minion_Holds(World& w, Entity& m, Entity* t)
{
	return t && t->team != m.team &&
	       VDist(m.pos, t->pos) <= w.cfg.detectRange + w.cfg.targetLeash;
}

void AI_DecideMinion(World& w, Entity& m)
{
	const Config& cfg = w.cfg;

	// --- 1. Transitions out of the current state ---------------------------
	// A fresh enemy-champion attack pre-empts every state and forces retaliation.
	if (Entity* threat = Minion_NewThreat(w, m))
	{
		m.state           = STATE_THREATENED;
		m.target          = m.lastAttacker;
		m.reactedAttacker = m.lastAttacker; // answered once; won't re-trigger
	}
	else switch (m.state)
	{
	case STATE_MARCHING:
	{
		EntityId id;
		if (Minion_Acquire(w, m, id)) { m.state = STATE_IN_COMBAT; m.target = id; }
		break;
	}
	case STATE_IN_COMBAT:
	{
		// Hold the target while valid; else re-acquire, or fall back to marching.
		if (!Minion_Holds(w, m, World_Get(w, m.target)))
		{
			EntityId id;
			if (Minion_Acquire(w, m, id)) m.target = id;
			else { m.state = STATE_MARCHING; m.target = InvalidId(); }
		}
		break;
	}
	case STATE_THREATENED:
	{
		// Stay locked on the champion until it leaves leash or memory fades,
		// then drop to combat (enemy still near) or marching (lane clear).
		Entity* atk = World_Get(w, m.target);
		bool hold = atk && atk->team != m.team && m.lastAttackerTimer > 0.0f &&
		            VDist(m.pos, atk->pos) <= cfg.detectRange + cfg.targetLeash;
		if (!hold)
		{
			EntityId id;
			if (Minion_Acquire(w, m, id)) { m.state = STATE_IN_COMBAT; m.target = id; }
			else { m.state = STATE_MARCHING; m.target = InvalidId(); }
		}
		break;
	}
	}

	// --- 2. Behaviour for the resulting state ------------------------------
	Entity*   tgt = World_Get(w, m.target);
	CP_Vector desired, dir;

	if (m.state == STATE_MARCHING || !tgt)
	{
		// No target: walk straight down the lane toward the enemy base. Local
		// avoidance + separation below handle flowing around other units.
		m.state = STATE_MARCHING; // guard: a stale handle => genuinely no target
		m.target = InvalidId();
		CP_Vector goal = World_EnemyBasePoint(w, m.team);
		dir     = VNorm(VSub(goal, m.pos));
		desired = Steer_Arrive(m.pos, goal, m.moveSpeed, 2.0f, cfg.arriveRadius);
	}
	else
	{
		// IN_COMBAT / THREATENED: close on the committed target, stopping at
		// attack range so Phase_Act can fire. Same movement, different intent.
		float stopRadius = m.attackRange + tgt->radius;
		dir     = VNorm(VSub(tgt->pos, m.pos));

		// Once in range, plant: a minion mid-attack holds its ground and only
		// moves again if the target leaves range. Skipping separation/avoidance
		// here (they run below for moving minions) stops the fighting front from
		// drifting as the scrum shoves it around.
		if (VDist(m.pos, tgt->pos) <= stopRadius)
		{
			m.vel = CP_Vector_Zero();
			return;
		}
		desired = Steer_Arrive(m.pos, tgt->pos, m.moveSpeed, stopRadius, cfg.arriveRadius);
	}

	// Local avoidance (only while actually moving, so settled attackers don't jitter)
	// plus separation on top of either mode keeps units from piling up. The brake
	// slows the forward push when a blocker is head-on so minions ease around it.
	CP_Vector avoid = CP_Vector_Zero();
	float     brake = 1.0f;
	if (VLen(desired) > 1.0f)
		avoid = Steer_Avoid(w, m, m.target, dir, cfg.avoidLookahead, cfg.avoidStrength, brake);
	CP_Vector sep = Steer_Separation(w, m, cfg.separationRange, cfg.separationStrength);
	m.vel = VLimit(VAdd(VAdd(VScale(desired, brake), avoid), sep), m.moveSpeed);
}

// ---------------------------------------------------------------------------
// Tower: lock a target and hold it until dead or out of range; minions first,
// champion only when no minion is available (the seed of aggro manipulation).
// ---------------------------------------------------------------------------
void AI_DecideTower(World& w, Entity& t)
{
	t.vel = CP_Vector_Zero();

	Entity* cur = World_Get(w, t.target);
	bool keep = cur && cur->team != t.team &&
	            VDist(t.pos, cur->pos) <= t.attackRange + cur->radius;

	if (!keep)
	{
		EntityId id;
		Entity* e = World_NearestEnemy(w, t, KIND_MINION, t.attackRange, &id);
		if (!e)
			e = World_NearestEnemy(w, t, KIND_CHAMPION, t.attackRange, &id);
		t.target = e ? id : InvalidId();
	}
}

// ---------------------------------------------------------------------------
// Champion: player-driven via the mouse. A right-click either sets an attack-move
// order (clicked an enemy) or an A*-pathed move order (clicked the ground). Each
// tick we advance toward that standing order; Phase_Act does the actual attacking.
// ---------------------------------------------------------------------------
void AI_DecideChampion(World& w, Entity& c, const SimInput& input)
{
	const Config& cfg = w.cfg;

	// 1. Apply a new order, if one was issued this tick.
	if (input.issued)
	{
		Entity* clicked = World_Get(w, input.targetEntity);
		if (clicked && clicked->team != c.team)
		{
			w.champOrder   = input.targetEntity; // attack-move to this enemy
			w.champHasGoal = false;
			w.champPath.clear();
		}
		else
		{
			w.champOrder   = InvalidId();        // move to the clicked ground point
			w.champGoal    = input.worldPoint;
			w.champHasGoal = Nav_FindPath(w.nav, c.pos, input.worldPoint, w.champPath);
			w.champPathIdx = 0;
		}
	}

	// 2. Execute the standing order.
	CP_Vector desired = CP_Vector_Zero();
	c.target = InvalidId();

	Entity* order = World_Get(w, w.champOrder);
	if (order && order->team != c.team)
	{
		// Walk toward the ordered enemy; attack once it is in range.
		float stopRadius = c.attackRange + order->radius;
		desired = Steer_Arrive(c.pos, order->pos, c.moveSpeed, stopRadius, cfg.arriveRadius);
		if (VDist(c.pos, order->pos) <= stopRadius)
			c.target = w.champOrder; // Phase_Act fires on cooldown
	}
	else if (w.champHasGoal && !w.champPath.empty())
	{
		// Follow the A* waypoints, popping each as we reach it.
		while (w.champPathIdx < w.champPath.size() &&
		       VDist(c.pos, w.champPath[w.champPathIdx]) < c.radius + 6.0f)
			++w.champPathIdx;

		if (w.champPathIdx >= w.champPath.size())
			w.champHasGoal = false; // arrived
		else
			desired = Steer_Arrive(c.pos, w.champPath[w.champPathIdx],
			                       c.moveSpeed, 2.0f, cfg.arriveRadius);
	}

	c.vel   = VLimit(desired, c.moveSpeed);
	c.state = STATE_MARCHING;
}
