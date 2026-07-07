#include "Analysis.h"

#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Read-only observer. Nothing in this file writes to the World; it only samples
// it. Keeping the measurement out of the sim means analysis can never perturb a
// run's outcome, and the influence field / detectors can be toggled freely.
// ---------------------------------------------------------------------------

const char* Technique_Name(Technique t)
{
	switch (t)
	{
	case TECH_FREEZE:       return "FREEZE";
	case TECH_SLOW_PUSH:    return "SLOW PUSH";
	case TECH_FAST_PUSH:    return "FAST PUSH";
	case TECH_TOWER_AGGRO:  return "TOWER AGGRO";
	case TECH_MINION_BLOCK: return "MINION BLOCK";
	default:                return "";
	}
}

// How loud each technique is: a more deliberate/specific play wins the banner if
// several fire on the same frame.
static int Technique_Priority(Technique t)
{
	switch (t)
	{
	case TECH_TOWER_AGGRO:  return 5;
	case TECH_MINION_BLOCK: return 4;
	case TECH_FREEZE:       return 3;
	case TECH_FAST_PUSH:    return 2;
	case TECH_SLOW_PUSH:    return 1;
	default:                return 0;
	}
}

// Const entity lookup (World_Get is non-const; this observer holds const World).
static const Entity* Get(const World& w, EntityId id)
{
	if (id.index == INVALID_INDEX || id.index >= w.ents.size())
		return nullptr;
	const Entity& e = w.ents[id.index];
	if (!e.alive || e.id.gen != id.gen)
		return nullptr;
	return &e;
}

static float PresenceWeight(const World& w, const Entity& e)
{
	switch (e.kind)
	{
	case KIND_MINION:   return w.cfg.influenceMinionW * (e.minionType == MINION_CANNON ? 1.5f : 1.0f);
	case KIND_CHAMPION: return w.cfg.influenceChampW;
	case KIND_TOWER:    return w.cfg.influenceTowerW;
	default:            return 0.0f;
	}
}

// ---------------------------------------------------------------------------
// 1. Influence field: splat every unit's presence into the lane columns using a
//    triangular kernel centred on its along-lane position.
// ---------------------------------------------------------------------------
static void BuildInfluence(const World& w, AnalysisState& a)
{
	const int cols = w.cfg.influenceCols > 1 ? w.cfg.influenceCols : 2;
	a.cols = cols;
	a.blueInfluence.assign(cols, 0.0f);
	a.redInfluence.assign(cols, 0.0f);
	a.netInfluence.assign(cols, 0.0f);
	a.blueMinions = 0;
	a.redMinions  = 0;

	const float spread = w.cfg.influenceSpread > 0.0001f ? w.cfg.influenceSpread : 0.02f;

	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		const Entity& e = w.ents[i];
		if (!e.alive || e.team == TEAM_NEUTRAL)
			continue;
		float weight = PresenceWeight(w, e);
		if (weight <= 0.0f)
			continue;

		if (e.kind == KIND_MINION)
			(e.team == TEAM_BLUE ? a.blueMinions : a.redMinions)++;

		// Along-lane position, clamped into [0,1] so units at the very ends still land.
		float t = Lane_Coord(w.lane, e.pos);
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;

		std::vector<float>& field = (e.team == TEAM_BLUE) ? a.blueInfluence : a.redInfluence;
		for (int c = 0; c < cols; ++c)
		{
			float ct = (c + 0.5f) / cols;
			float d  = std::fabs(ct - t);
			if (d >= spread)
				continue;
			field[c] += weight * (1.0f - d / spread); // triangular falloff
		}
	}

	a.maxAbsInfluence = 0.0001f;
	for (int c = 0; c < cols; ++c)
	{
		a.netInfluence[c] = a.blueInfluence[c] - a.redInfluence[c];
		float m = std::fabs(a.netInfluence[c]);
		if (m > a.maxAbsInfluence)
			a.maxAbsInfluence = m;
	}
}

// ---------------------------------------------------------------------------
// 2. Equilibrium point: the lane boundary that best sorts blue-controlled space
//    to the left and red-controlled space to the right. We pick the split that
//    minimises "misplaced" mass (red left of it + blue right of it). As blue's
//    presence extends toward the red base the boundary slides right, so the
//    equilibrium's drift is exactly the push/pull the player feels.
// ---------------------------------------------------------------------------
static void FindEquilibrium(const World& w, AnalysisState& a)
{
	const int cols = a.cols;
	float totalBlue = 0.0f, totalRed = 0.0f;
	for (int c = 0; c < cols; ++c) { totalBlue += a.blueInfluence[c]; totalRed += a.redInfluence[c]; }

	a.equilibriumValid = (totalBlue > 0.001f && totalRed > 0.001f);
	if (!a.equilibriumValid)
		return;

	// Boundary b lies just after column b (b = -1 => everything on the red side).
	// score(b) = red mass at/left of b + blue mass right of b. We minimise it.
	// The boundary's world position after column b is (b+1)/cols.
	//
	// Two passes: first find the best score, then take the MIDPOINT of the plateau of
	// boundaries that tie for it. This matters when the lane centre is empty (only the
	// towers exist at the start): every split through the empty middle has zero
	// misplaced mass, so they all tie, and picking the first would park the marker at
	// the left edge of that gap - on the blue side - instead of dead centre. Once the
	// waves actually meet, the tie collapses to the real crossing, so combat is
	// unaffected.
	// Pass 1: find the best (minimum) score, boundary b = -1 (all red side) included.
	float prefixRed = 0.0f, prefixBlue = 0.0f;
	float bestScore = totalBlue; // b = -1: all blue counts as "on the red side"
	for (int b = 0; b < cols; ++b)
	{
		prefixRed  += a.redInfluence[b];
		prefixBlue += a.blueInfluence[b];
		float score = prefixRed + (totalBlue - prefixBlue);
		if (score < bestScore)
			bestScore = score;
	}

	// Pass 2: span of boundaries that tie for that score, so we can take its midpoint.
	const float eps = (totalBlue + totalRed) * 1e-4f + 1e-4f;
	int firstB = (totalBlue <= bestScore + eps) ? -1 : cols;
	int lastB  = (totalBlue <= bestScore + eps) ? -1 : -2;
	prefixRed = prefixBlue = 0.0f;
	for (int b = 0; b < cols; ++b)
	{
		prefixRed  += a.redInfluence[b];
		prefixBlue += a.blueInfluence[b];
		float score = prefixRed + (totalBlue - prefixBlue);
		if (score <= bestScore + eps)
		{
			if (b < firstB) firstB = b;
			if (b > lastB)  lastB  = b;
		}
	}

	// Midpoint of the tied boundary range, each boundary sitting at (b+1)/cols.
	a.equilibriumT = ((firstB + 1) + (lastB + 1)) * 0.5f / (float)cols;
}

// ---------------------------------------------------------------------------
// 3. Detectors: name the emergent techniques from the field + equilibrium drift.
// ---------------------------------------------------------------------------
void Analysis_Update(const World& w, AnalysisState& a, float simElapsed)
{
	const Config& cfg = w.cfg;

	BuildInfluence(w, a);
	FindEquilibrium(w, a);

	// Fade the banner and cool detectors down on sim time (so pausing holds them).
	if (a.banner.ttl > 0.0f)
	{
		a.banner.ttl -= simElapsed;
		if (a.banner.ttl < 0.0f) a.banner.ttl = 0.0f;
	}
	for (int i = 0; i < TECH_COUNT; ++i)
		if (a.cooldown[i] > 0.0f)
		{
			a.cooldown[i] -= simElapsed;
			if (a.cooldown[i] < 0.0f) a.cooldown[i] = 0.0f;
		}

	// Smoothed drift of the equilibrium point (only when the sim actually advanced).
	if (a.equilibriumValid && simElapsed > 0.0f)
	{
		if (a.havePrev)
		{
			float raw = (a.equilibriumT - a.prevEquilibriumT) / simElapsed;
			a.equilibriumVel += cfg.equilVelSmoothing * (raw - a.equilibriumVel);
		}
		a.prevEquilibriumT = a.equilibriumT;
		a.havePrev = true;
	}

	// Detectors run on sim time; while paused we only keep the field visible.
	if (simElapsed <= 0.0f)
		return;

	// Announce a technique, respecting its cooldown and letting the loudest of any
	// simultaneous events own the banner.
	auto announce = [&](Technique t, Team by)
	{
		if (a.cooldown[t] > 0.0f)
			return;
		if (a.banner.ttl <= 0.0f || Technique_Priority(t) >= Technique_Priority(a.banner.tech))
		{
			a.banner.tech   = t;
			a.banner.ttl    = cfg.bannerTtl;
			a.banner.byTeam = by;
		}
		a.cooldown[t] = cfg.techCooldown;
	};

	// --- Wave-management techniques, read off the equilibrium velocity ---------
	const float vel    = a.equilibriumVel;
	const float absVel = std::fabs(vel);
	const int   dir    = (vel > 0.0f) ? +1 : -1; // +1 = front moving toward red base

	if (absVel < cfg.freezeVelEps)
	{
		// Front held stationary. A balanced stalemate at mid-lane is just natural
		// equilibrium; a freeze is holding the wave off-centre, on one side's half.
		a.freezeTimer += simElapsed;
		a.pushTimer = 0.0f; a.pushDir = 0;
		if (a.freezeTimer >= cfg.freezeHoldTime && std::fabs(a.equilibriumT - 0.5f) > 0.06f)
			announce(TECH_FREEZE, a.equilibriumT < 0.5f ? TEAM_BLUE : TEAM_RED);
	}
	else
	{
		a.freezeTimer = 0.0f;
		if (absVel >= cfg.slowPushVel)
		{
			// A sustained directional drift is a push; hold it a moment to be sure.
			if (dir == a.pushDir) a.pushTimer += simElapsed;
			else { a.pushDir = dir; a.pushTimer = simElapsed; }

			if (a.pushTimer >= cfg.pushHoldTime)
			{
				Team by = (dir > 0) ? TEAM_BLUE : TEAM_RED;
				announce(absVel >= cfg.fastPushVel ? TECH_FAST_PUSH : TECH_SLOW_PUSH, by);
			}
		}
		else
		{
			a.pushTimer = 0.0f; a.pushDir = 0; // creeping: too slow to call a push
		}
	}

	// --- Tower-aggro manipulation: the enemy tower is hitting your champion ------
	const Entity* champ    = Get(w, w.champion);
	const Entity* redTower = Get(w, w.redTower);
	if (champ && redTower && SameId(redTower->target, w.champion))
		announce(TECH_TOWER_AGGRO, TEAM_BLUE);

	// --- Minion block: your champion is stood in the enemy wave's path -----------
	if (champ)
	{
		CP_Vector redTravel = VNorm(VSub(w.lane.blueBase, w.lane.redBase)); // red marches this way
		int blocked = 0;
		for (size_t i = 0; i < w.ents.size(); ++i)
		{
			const Entity& e = w.ents[i];
			if (!e.alive || e.kind != KIND_MINION || e.team != TEAM_RED)
				continue;
			if (VDist(e.pos, champ->pos) > cfg.blockRadius)
				continue;
			// Champion counts as a block only when it sits ahead of the minion along
			// that minion's line of march (i.e. between it and its goal).
			CP_Vector toChamp = VSub(champ->pos, e.pos);
			if (toChamp.x * redTravel.x + toChamp.y * redTravel.y > 0.0f)
				++blocked;
		}
		if (blocked >= cfg.blockMinCount)
			announce(TECH_MINION_BLOCK, TEAM_BLUE);
	}
}
