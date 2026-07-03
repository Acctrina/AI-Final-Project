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
// Minion: fixed aggro priority + 3-state FSM, then steer toward the goal.
// ---------------------------------------------------------------------------
void AI_DecideMinion(World& w, Entity& m)
{
	const Config& cfg = w.cfg;

	Entity*  tgt   = nullptr;
	EntityId tgtId = InvalidId();
	FsmState state = STATE_MARCHING;

	// Is the current target still worth holding? Stickiness/hysteresis, like the tower:
	// commit to a target until it dies or leaves leash range, instead of re-picking
	// "closest" every tick (which made minions thrash between targets mid-fight).
	Entity* cur = World_Get(w, m.target);
	bool holdCurrent = cur && cur->team != m.team &&
	                   VDist(m.pos, cur->pos) <= cfg.detectRange + cfg.targetLeash;

	// Priority 1 (retaliation): only an enemy *champion* attacking pulls a minion off
	// its target - enemy minion pokes (e.g. a caster outranging our melee) must NOT, or
	// the whole wave chases casters instead of fighting the front line. And only a *new*
	// champion attacker re-triggers, so continuous fire doesn't thrash the target.
	Entity* atk = World_Get(w, m.lastAttacker);
	bool newAttacker = atk && atk->team != m.team && atk->kind == KIND_CHAMPION &&
	                   m.lastAttackerTimer > 0.0f &&
	                   !SameId(m.lastAttacker, m.reactedAttacker) &&
	                   !SameId(m.lastAttacker, m.target);

	if (newAttacker)
	{
		tgt = atk; tgtId = m.lastAttacker;
		state = STATE_THREATENED;
		m.reactedAttacker = m.lastAttacker; // reacted once; won't re-trigger for this attacker
	}
	else if (holdCurrent)
	{
		tgt = cur; tgtId = m.target;
		state = (m.lastAttackerTimer > 0.0f && SameId(m.target, m.lastAttacker))
		        ? STATE_THREATENED : STATE_IN_COMBAT;
	}
	else
	{
		// No valid target held: acquire fresh via the fixed priority list
		// (attacking enemy champion, then closest minion, champion, tower).
		if (atk && atk->team != m.team && atk->kind == KIND_CHAMPION && m.lastAttackerTimer > 0.0f)
		{
			tgt = atk; tgtId = m.lastAttacker; state = STATE_THREATENED;
			m.reactedAttacker = m.lastAttacker;
		}
		if (!tgt)
		{
			tgt = World_NearestEnemy(w, m, KIND_MINION, cfg.detectRange, &tgtId);
			if (tgt) state = STATE_IN_COMBAT;
		}
		if (!tgt)
		{
			tgt = World_NearestEnemy(w, m, KIND_CHAMPION, cfg.detectRange, &tgtId);
			if (tgt) state = STATE_IN_COMBAT;
		}
		if (!tgt)
		{
			tgt = World_NearestEnemy(w, m, KIND_TOWER, cfg.detectRange, &tgtId);
			if (tgt) state = STATE_IN_COMBAT;
		}
	}

	m.target = tgtId;
	m.state  = tgt ? state : STATE_MARCHING;

	CP_Vector desired, dir;

	if (tgt)
	{
		// In combat: steer straight at the target (it's close), stopping at attack range.
		CP_Vector goal = tgt->pos;
		float stopRadius = m.attackRange + tgt->radius;
		CP_Vector to = VSub(goal, m.pos);
		dir = VNorm(to);
		desired = Steer_Arrive(m.pos, goal, m.moveSpeed, stopRadius, cfg.arriveRadius);
	}
	else
	{
		// Marching: steer straight down the lane toward the enemy base. Local
		// avoidance + separation below handle flowing around other units.
		m.state = STATE_MARCHING;
		CP_Vector goal = World_EnemyBasePoint(w, m.team);
		dir = VNorm(VSub(goal, m.pos));
		desired = Steer_Arrive(m.pos, goal, m.moveSpeed, 2.0f, cfg.arriveRadius);
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
