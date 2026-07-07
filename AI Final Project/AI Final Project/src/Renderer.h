#pragma once

// Read-only view over the World. Every CProcessing draw call in the project lives
// behind this layer; the sim itself never draws. Debug overlays are toggled by the
// caller (main.cpp) via RenderOptions.

#include "Sim.h"
#include "Analysis.h"

struct RenderOptions
{
	bool showAggroLines;   // line from each agent to its current target
	bool showRanges;       // detection / attack range rings
	bool paused;
	bool showInfluence;    // lane influence heatmap (analysis layer)
	bool showEquilibrium;  // wave-equilibrium marker + technique banner
};

void Render_World(World& w, const AnalysisState& analysis, const RenderOptions& opt);
