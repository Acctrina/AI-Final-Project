#pragma once

// Read-only view over the World. Every CProcessing draw call in the project lives
// behind this layer; the sim itself never draws. Debug overlays are toggled by the
// caller (main.cpp) via RenderOptions.

#include "Sim.h"

struct RenderOptions
{
	bool showAggroLines;   // line from each agent to its current target
	bool showRanges;       // detection / attack range rings
	bool paused;
};

void Render_World(World& w, const RenderOptions& opt);
