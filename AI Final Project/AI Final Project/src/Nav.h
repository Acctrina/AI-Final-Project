#pragma once

// Grid-based navigation for the champion's mouse click-to-move: an A* search over a
// static walkability grid built once from the lane band + tower footprints. It does
// not track moving units - minion crowding is handled by local steering in AI.cpp.

#include <vector>
#include "Config.h"

struct World; // defined in Sim.h; Nav.cpp includes it

struct NavGrid
{
	int   cols = 0;
	int   rows = 0;
	float cellSize = 24.0f;
	float originX = 0.0f;
	float originY = 0.0f;

	std::vector<unsigned char> blocked;  // cols*rows, 1 = not walkable
};

// Build the walkability grid from the world's static geometry (lane band + towers).
void Nav_Build(const World& w, NavGrid& nav);

// A* from start to goal, snapping either endpoint to the nearest walkable cell.
// Fills out with world-space waypoints (cell centres). Returns false if no path.
bool Nav_FindPath(const NavGrid& nav, CP_Vector start, CP_Vector goal, std::vector<CP_Vector>& out);
