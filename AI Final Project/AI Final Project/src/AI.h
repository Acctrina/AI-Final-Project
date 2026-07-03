#pragma once

// The "decide" phase - the actual (intentionally minimal) agent brains. Given the
// world state from the previous tick, each function picks a target, sets an FSM
// state, and writes a desired velocity onto the entity. It reads state; the "act"
// and "resolve" phases in Sim.cpp are what mutate the world.

#include "Sim.h"

// Minion: 3-state FSM + fixed aggro-priority list + steering.
void AI_DecideMinion(World& w, Entity& m);

// Tower: locks a target and holds it until dead / out of range.
void AI_DecideTower(World& w, Entity& t);

// Champion: driven by player input rather than autonomy.
void AI_DecideChampion(World& w, Entity& c, const SimInput& input);
