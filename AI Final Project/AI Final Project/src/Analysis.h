#pragma once

// The analysis / emergence layer: a read-only observer over the World. It never
// mutates the sim, so it cannot change the outcome of a run - it only measures it.
//
// Each update it (1) samples unit presence into columns along the lane to build an
// influence field, (2) locates the wave-equilibrium point where the two sides
// balance, and (3) runs a set of detectors that RECOGNISE emergent player
// techniques from that state. The techniques are not features we coded into the
// game; they fall out of the minion rules, and this layer merely names them when
// it sees their signature - which is the whole thesis made visible.

#include <vector>
#include "Sim.h"

enum Technique
{
	TECH_NONE = 0,
	TECH_FREEZE,        // front held near-stationary on your side of the lane
	TECH_SLOW_PUSH,     // equilibrium drifting toward the enemy, slowly
	TECH_FAST_PUSH,     // equilibrium racing toward the enemy (a shove)
	TECH_TOWER_AGGRO,   // you pulled the enemy tower onto your champion
	TECH_MINION_BLOCK,  // your champion is body-blocking the enemy wave
	TECH_COUNT
};

const char* Technique_Name(Technique t);

// A recognised technique currently shown on screen, with a fading lifetime.
struct TechniqueEvent
{
	Technique tech   = TECH_NONE;
	float     ttl    = 0.0f;         // seconds of display left
	Team      byTeam = TEAM_NEUTRAL; // who performed it (blue = the player)
};

struct AnalysisState
{
	// Lane influence field, one entry per column (index 0 at the blue base ..
	// cols-1 at the red base). net = blue - red presence.
	int                cols = 0;
	std::vector<float> blueInfluence;
	std::vector<float> redInfluence;
	std::vector<float> netInfluence;
	float              maxAbsInfluence = 1.0f; // normaliser for the heatmap

	// Wave equilibrium: the lane parameter (0..1) where net influence crosses zero,
	// i.e. where the two waves meet. Velocity is the smoothed drift of that point,
	// in lane-fractions per second (positive = the front moving toward the red base).
	bool  equilibriumValid = false;
	float equilibriumT     = 0.5f;
	float equilibriumVel   = 0.0f;

	int   blueMinions = 0;
	int   redMinions  = 0;

	// Rolling detector memory.
	float prevEquilibriumT = 0.5f;
	bool  havePrev         = false;
	float freezeTimer      = 0.0f; // how long the front has been held
	float pushTimer        = 0.0f; // how long a push has persisted
	int   pushDir          = 0;    // sign of the sustained push (+1 blue, -1 red)

	// The banner currently shown (the highest-priority active event) and a per-
	// technique cooldown so a detector does not re-announce itself every frame.
	TechniqueEvent banner;
	float          cooldown[TECH_COUNT] = {};
};

// Advance the analysis by simElapsed seconds of *sim* time (0 while paused, so the
// detectors are pause-aware and independent of the real frame rate).
void Analysis_Update(const World& w, AnalysisState& a, float simElapsed);
