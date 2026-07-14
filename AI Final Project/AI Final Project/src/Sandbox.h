#pragma once

#include "Sim.h"
#include "Renderer.h"

struct SandboxState
{
    uint32_t seed = 1337u;
    const char* currentScenario = "Neutral";

    bool allowGameMouseInput = true;
    bool allowGameKeyboardInput = true;

    // Optional future toggles.
    bool autoResetOnScenario = true;
};

void Sandbox_ApplyImGuiTheme();

void Sandbox_ResetWorld(World& w, float& accum, bool& paused, uint32_t seed);
void Sandbox_ResetConfigToDefaults(World& w, float& accum, bool& paused, uint32_t seed);
void Sandbox_ClearMinions(World& w);
void Sandbox_ClearProjectiles(World& w);
void Sandbox_ResetWaveTimers(World& w, float nextInSeconds);
void Sandbox_ForceNextWave(World& w, Team team);
void Sandbox_ForceBothWaves(World& w);
void Sandbox_KillAllMinionsOfTeam(World& w, Team team);

// Scenario stubs / first-pass setups.
void Sandbox_LoadNeutral(World& w, float& accum, bool& paused, uint32_t seed);
void Sandbox_LoadFreeze(World& w, float& accum, bool& paused, uint32_t seed);
void Sandbox_LoadSlowPush(World& w, float& accum, bool& paused, uint32_t seed);
void Sandbox_LoadShove(World& w, float& accum, bool& paused, uint32_t seed);
void Sandbox_LoadTowerAggro(World& w, float& accum, bool& paused, uint32_t seed);

// Draws the ImGui sandbox panel.
void Sandbox_DrawImGui(World& w, RenderOptions& render, SandboxState& sandbox, float& accum, bool& paused);
