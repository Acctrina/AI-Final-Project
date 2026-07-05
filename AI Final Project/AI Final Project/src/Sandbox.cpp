#include "Sandbox.h"

#include "imgui.h"

#include <cfloat>

// ----------------------------------------------------------------------------
// Private helpers
// ----------------------------------------------------------------------------

// Count living entities of a given kind, optionally filtered by team.
static int CountAlive(const World& w, EntityKind kind, Team team)
{
    int count = 0;
    for (size_t i = 0; i < w.ents.size(); ++i)
    {
        const Entity& e = w.ents[i];
        if (!e.alive) continue;
        if (e.kind != kind) continue;
        if (team != TEAM_NEUTRAL && e.team != team) continue;
        ++count;
    }
    return count;
}

// Convenient access to the player champion entity.
static Entity* GetChampion(World& w)
{
    return World_Get(w, w.champion);
}

// Clear pending wave spawns so scenarios start from a controlled state.
static void ClearWaveQueues(World& w)
{
    for (int t = 0; t < 2; ++t)
    {
        w.waves[t].queue.clear();
        w.waves[t].spawnTimer = 0.0f;
    }
}

// Small stylized section header used between major UI groups.
static void Sandbox_DrawSectionHeader(const char* title)
{
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(ImVec4(0.62f, 0.78f, 1.0f, 1.0f), "%s", title);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
}

// Wrapper for collapsible tool sections.
static bool Sandbox_BeginCard(const char* label, ImGuiTreeNodeFlags extraFlags = 0)
{
    return ImGui::CollapsingHeader(label, extraFlags);
}

// Compute half-width buttons with spacing accounted for.
static float Sandbox_HalfButtonWidth()
{
    const float full = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    return (full - gap) * 0.5f;
}

// Begin a 2-column property grid: label on the left, control on the right.
static bool Sandbox_BeginPropertyTable(const char* id)
{
    return ImGui::BeginTable(
        id,
        2,
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg);
}

static void Sandbox_EndPropertyTable()
{
    ImGui::EndTable();
}

// Emit the left-side label cell, then prepare the right cell for a full-width widget.
static void Sandbox_PropertyLabel(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

static bool Sandbox_SliderFloat(const char* label, const char* id, float* v, float minV, float maxV, const char* fmt = "%.3f")
{
    Sandbox_PropertyLabel(label);
    return ImGui::SliderFloat(id, v, minV, maxV, fmt);
}

static bool Sandbox_SliderInt(const char* label, const char* id, int* v, int minV, int maxV, const char* fmt = "%d")
{
    Sandbox_PropertyLabel(label);
    return ImGui::SliderInt(id, v, minV, maxV, fmt);
}

// Build a minion entity from config data
static Entity MakeScenarioMinion(const Config& c, Team team, MinionType type, CP_Vector pos, float hpScale = 1.0f)
{
    Entity e = {};
    e.id = InvalidId();
    e.kind = KIND_MINION;
    e.team = team;
    e.pos = pos;
    e.vel = CP_Vector_Zero();
    e.minionType = type;
    e.state = STATE_MARCHING;
    e.cooldownTimer = 0.0f;
    e.target = InvalidId();
    e.lastAttacker = InvalidId();
    e.reactedAttacker = InvalidId();
    e.source = InvalidId();
    e.projSpeed = c.projSpeed;

    switch (type)
    {
    case MINION_MELEE:
        e.maxHp = c.meleeHp;
        e.attackDamage = c.meleeDmg;
        e.attackRange = c.meleeRange;
        e.attackCooldown = c.meleeCooldown;
        e.moveSpeed = c.meleeSpeed;
        e.radius = c.meleeRadius;
        break;

    case MINION_CASTER:
        e.maxHp = c.casterHp;
        e.attackDamage = c.casterDmg;
        e.attackRange = c.casterRange;
        e.attackCooldown = c.casterCooldown;
        e.moveSpeed = c.casterSpeed;
        e.radius = c.casterRadius;
        break;

    case MINION_CANNON:
        e.maxHp = c.cannonHp;
        e.attackDamage = c.cannonDmg;
        e.attackRange = c.cannonRange;
        e.attackCooldown = c.cannonCooldown;
        e.moveSpeed = c.cannonSpeed;
        e.radius = c.cannonRadius;
        break;
    }

    e.hp = e.maxHp * hpScale;
    if (e.hp < 1.0f) e.hp = 1.0f;
    return e;
}

// Spawn a minion at a normalized lane position.
static void SpawnScenarioMinion(World& w, Team team, MinionType type, float laneT, float hpScale = 1.0f)
{
    Entity e = MakeScenarioMinion(w.cfg, team, type, Lane_PointAt(w.lane, laneT), hpScale);
    World_Spawn(w, e);
}

// Reposition the champion for a scenario setup.
static void PlaceChampionAt(World& w, float laneT)
{
    Entity* champ = GetChampion(w);
    if (!champ) return;

    champ->pos = Lane_PointAt(w.lane, laneT);
    champ->target = InvalidId();
}

// Reset into a paused, controlled sandbox state
static void ResetScenarioBase(World& w, float& accum, bool& paused, uint32_t seed)
{
    World_Init(w, seed);
    accum = 0.0f;
    paused = true;
    ClearWaveQueues(w);

    // Delay normal automatic waves so the preset setup can be seen first.
    for (int t = 0; t < 2; ++t)
        w.waves[t].nextWaveTimer = 9999.0f;
}

// ----------------------------------------------------------------------------
// ImGui Theme
// ----------------------------------------------------------------------------

// Custom dark theme for the sandbox panel.
void Sandbox_ApplyImGuiTheme()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(10, 6);
    style.CellPadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 10.0f;

    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 8.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.57f, 0.64f, 1.00f);

    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.14f, 0.17f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.16f, 0.98f);

    c[ImGuiCol_Border] = ImVec4(0.24f, 0.28f, 0.34f, 0.60f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.11f, 0.14f, 0.90f);

    c[ImGuiCol_Button] = ImVec4(0.21f, 0.36f, 0.62f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.44f, 0.74f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.31f, 0.54f, 1.00f);

    c[ImGuiCol_Header] = ImVec4(0.18f, 0.21f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.27f, 0.33f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.31f, 0.38f, 1.00f);

    c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.73f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.73f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.67f, 0.84f, 1.00f, 1.00f);

    c[ImGuiCol_Separator] = ImVec4(0.24f, 0.28f, 0.34f, 0.70f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0.21f, 0.36f, 0.62f, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.21f, 0.36f, 0.62f, 0.70f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0.21f, 0.36f, 0.62f, 1.00f);

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.26f, 0.30f, 0.36f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.23f, 0.28f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
}

// ----------------------------------------------------------------------------
// Sandbox controls
// ----------------------------------------------------------------------------

// Full world reset used by the main reset button.
void Sandbox_ResetWorld(World& w, float& accum, bool& paused, uint32_t seed)
{
    World_Init(w, seed);
    accum = 0.0f;
    paused = false;
}

// Remove all current minions from the world.
void Sandbox_ClearMinions(World& w)
{
    for (size_t i = 0; i < w.ents.size(); ++i)
    {
        Entity& e = w.ents[i];
        if (!e.alive) continue;
        if (e.kind == KIND_MINION)
            World_Destroy(w, e.id);
    }
    ClearWaveQueues(w);
}

// Remove all active projectiles.
void Sandbox_ClearProjectiles(World& w)
{
    for (size_t i = 0; i < w.ents.size(); ++i)
    {
        Entity& e = w.ents[i];
        if (!e.alive) continue;
        if (e.kind == KIND_PROJECTILE)
            World_Destroy(w, e.id);
    }
}

// Reset both team's wave timers to a known value.
void Sandbox_ResetWaveTimers(World& w, float nextInSeconds)
{
    for (int t = 0; t < 2; ++t)
    {
        w.waves[t].nextWaveTimer = nextInSeconds;
        w.waves[t].spawnTimer = 0.0f;
        w.waves[t].queue.clear();
    }
}

// Force a team's next wave to spawn immediately.
void Sandbox_ForceNextWave(World& w, Team team)
{
    Wave& wave = w.waves[(int)team];
    wave.queue.clear();
    wave.spawnTimer = 0.0f;
    wave.nextWaveTimer = 0.0f;
}

void Sandbox_ForceBothWaves(World& w)
{
    Sandbox_ForceNextWave(w, TEAM_BLUE);
    Sandbox_ForceNextWave(w, TEAM_RED);
}

// Destroy all minions belonging to one team.
void Sandbox_KillAllMinionsOfTeam(World& w, Team team)
{
    for (size_t i = 0; i < w.ents.size(); ++i)
    {
        Entity& e = w.ents[i];
        if (!e.alive) continue;
        if (e.kind != KIND_MINION) continue;
        if (e.team != team) continue;
        World_Destroy(w, e.id);
    }
}

// ----------------------------------------------------------------------------
// Scenarios (Still WIP)
// ----------------------------------------------------------------------------

// Baseline world state with normal spawning behavior.
void Sandbox_LoadNeutral(World& w, float& accum, bool& paused, uint32_t seed)
{
    Sandbox_ResetWorld(w, accum, paused, seed);
}

// Set up a wave freeze near the player's side of the lane.
void Sandbox_LoadFreeze(World& w, float& accum, bool& paused, uint32_t seed)
{
    ResetScenarioBase(w, accum, paused, seed);
    Sandbox_ClearMinions(w);
    PlaceChampionAt(w, 0.22f);

    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.26f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.272f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.284f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_CASTER, 0.24f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_CASTER, 0.252f);

    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.36f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.372f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.384f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.396f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.38f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.392f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.404f);
}

// Set up a blue-side slow push with a small wave advantage.
void Sandbox_LoadSlowPush(World& w, float& accum, bool& paused, uint32_t seed)
{
    ResetScenarioBase(w, accum, paused, seed);
    Sandbox_ClearMinions(w);

    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.42f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.432f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.444f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.456f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_CASTER, 0.40f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_CASTER, 0.412f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_CASTER, 0.424f);

    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.52f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.532f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.54f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.552f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.564f);
}

// Set up a fast push / shove by weakening parts of the opposing wave.
void Sandbox_LoadShove(World& w, float& accum, bool& paused, uint32_t seed)
{
    ResetScenarioBase(w, accum, paused, seed);
    Sandbox_ClearMinions(w);
    PlaceChampionAt(w, 0.48f);

    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.43f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.442f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.454f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_CASTER, 0.466f);

    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.54f, 0.30f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.552f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.564f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.576f, 0.50f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_CASTER, 0.588f, 0.50f);
}

// Set up a simple tower aggro demonstration near the enemy structure.
void Sandbox_LoadTowerAggro(World& w, float& accum, bool& paused, uint32_t seed)
{
    ResetScenarioBase(w, accum, paused, seed);
    Sandbox_ClearMinions(w);
    PlaceChampionAt(w, 0.63f);

    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.60f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.614f);
    SpawnScenarioMinion(w, TEAM_BLUE, MINION_MELEE, 0.628f);

    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.68f);
    SpawnScenarioMinion(w, TEAM_RED, MINION_MELEE, 0.692f);
}

// ----------------------------------------------------------------------------
// UI
// ----------------------------------------------------------------------------

void Sandbox_DrawImGui(World& w, RenderOptions& render, SandboxState& sandbox, float& accum, bool& paused)
{
    ImGui::SetNextWindowSize(ImVec2(480, 760), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Sandbox", nullptr, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    const float full = ImGui::GetContentRegionAvail().x;
    const float half = Sandbox_HalfButtonWidth();

    // ------------------------------------------------------------------------
    // Header
    // ------------------------------------------------------------------------
    {
        const ImVec4 stateColor = paused
            ? ImVec4(0.90f, 0.72f, 0.30f, 1.0f)
            : ImVec4(0.38f, 0.84f, 0.48f, 1.0f);

        ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.98f, 1.0f), "MOBA Lane Sandbox");
        ImGui::Spacing();

        if (ImGui::BeginTable("summary_strip", 5, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("State");
            ImGui::TextColored(stateColor, paused ? "Paused" : "Running");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted("FPS");
            ImGui::Text("%.1f", io.Framerate);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted("Tick");
            ImGui::Text("%lld", w.tick);

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted("Blue");
            ImGui::Text("%d", CountAlive(w, KIND_MINION, TEAM_BLUE));

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted("Red");
            ImGui::Text("%d", CountAlive(w, KIND_MINION, TEAM_RED));

            ImGui::EndTable();
        }

        ImGui::Dummy(ImVec2(0, 6));

        if (ImGui::Button(paused ? "Resume" : "Pause", ImVec2(half, 34)))
            paused = !paused;
        ImGui::SameLine();
        if (ImGui::Button("Step", ImVec2(half, 34)))
        {
            SimInput in = {};
            in.issued = false;
            in.worldPoint = CP_Vector_Zero();
            in.targetEntity = InvalidId();

            // Advance exactly one fixed simulation step while paused.
            Sim_Tick(w, w.cfg.fixedDt, in);
        }

        if (ImGui::Button("Reset World", ImVec2(full, 34)))
            Sandbox_ResetWorld(w, accum, paused, sandbox.seed);

        ImGui::Dummy(ImVec2(0, 4));

        if (Sandbox_BeginPropertyTable("HeaderProps"))
        {
            Sandbox_PropertyLabel("Seed");
            ImGui::InputScalar("##Seed", ImGuiDataType_U32, &sandbox.seed);
            Sandbox_EndPropertyTable();
        }
    }

    // ------------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------------
    Sandbox_DrawSectionHeader("Rendering");
    ImGui::Checkbox("Show aggro lines", &render.showAggroLines);
    ImGui::Checkbox("Show ranges", &render.showRanges);

    // ------------------------------------------------------------------------
    // Wave Control
    // ------------------------------------------------------------------------
    if (Sandbox_BeginCard("Wave Control"))
    {
        ImGui::PushID("WaveControl");

        if (Sandbox_BeginPropertyTable("WaveProps"))
        {
            Sandbox_SliderFloat("Wave interval", "##WaveInterval", &w.cfg.waveInterval, 2.0f, 30.0f, "%.2f s");
            Sandbox_SliderFloat("Spawn spacing", "##SpawnSpacing", &w.cfg.spawnSpacing, 0.05f, 2.0f, "%.2f s");
            Sandbox_SliderInt("Melee per wave", "##MeleePerWave", &w.cfg.meleePerWave, 0, 8);
            Sandbox_SliderInt("Caster per wave", "##CasterPerWave", &w.cfg.casterPerWave, 0, 8);
            Sandbox_SliderInt("Cannon every N wave", "##CannonEveryN", &w.cfg.cannonEveryNWaves, 0, 8);
            Sandbox_EndPropertyTable();
        }

        ImGui::Dummy(ImVec2(0, 4));

        if (ImGui::Button("Force Blue Wave", ImVec2(half, 30)))
            Sandbox_ForceNextWave(w, TEAM_BLUE);
        ImGui::SameLine();
        if (ImGui::Button("Force Red Wave", ImVec2(half, 30)))
            Sandbox_ForceNextWave(w, TEAM_RED);

        if (ImGui::Button("Force Both Waves", ImVec2(half, 30)))
            Sandbox_ForceBothWaves(w);
        ImGui::SameLine();
        if (ImGui::Button("Reset Timers", ImVec2(half, 30)))
            Sandbox_ResetWaveTimers(w, 1.0f);

        if (ImGui::Button("Clear Minions", ImVec2(half, 28)))
            Sandbox_ClearMinions(w);
        ImGui::SameLine();
        if (ImGui::Button("Clear Projectiles", ImVec2(half, 28)))
            Sandbox_ClearProjectiles(w);

        if (ImGui::Button("Kill Blue Minions", ImVec2(half, 28)))
            Sandbox_KillAllMinionsOfTeam(w, TEAM_BLUE);
        ImGui::SameLine();
        if (ImGui::Button("Kill Red Minions", ImVec2(half, 28)))
            Sandbox_KillAllMinionsOfTeam(w, TEAM_RED);

        ImGui::PopID();
    }

    // ------------------------------------------------------------------------
    // AI / Steering
    // ------------------------------------------------------------------------
    if (Sandbox_BeginCard("AI / Steering"))
    {
        ImGui::PushID("AISteering");

        if (Sandbox_BeginPropertyTable("AIProps"))
        {
            Sandbox_SliderFloat("Detect range", "##DetectRange", &w.cfg.detectRange, 80.0f, 400.0f, "%.1f");
            Sandbox_SliderFloat("Target leash", "##TargetLeash", &w.cfg.targetLeash, 0.0f, 200.0f, "%.1f");
            Sandbox_SliderFloat("Attacker memory", "##AttackerMemory", &w.cfg.attackerMemory, 0.0f, 8.0f, "%.2f s");
            Sandbox_SliderFloat("Arrive radius", "##ArriveRadius", &w.cfg.arriveRadius, 5.0f, 120.0f, "%.1f");
            Sandbox_SliderFloat("Separation range", "##SeparationRange", &w.cfg.separationRange, 0.0f, 120.0f, "%.1f");
            Sandbox_SliderFloat("Separation strength", "##SeparationStrength", &w.cfg.separationStrength, 0.0f, 120.0f, "%.1f");
            Sandbox_SliderFloat("Avoid lookahead", "##AvoidLookahead", &w.cfg.avoidLookahead, 10.0f, 200.0f, "%.1f");
            Sandbox_SliderFloat("Avoid strength", "##AvoidStrength", &w.cfg.avoidStrength, 0.0f, 160.0f, "%.1f");
            Sandbox_EndPropertyTable();
        }

        ImGui::PopID();
    }

    // ------------------------------------------------------------------------
    // Combat Tuning
    // ------------------------------------------------------------------------
    if (Sandbox_BeginCard("Combat Tuning"))
    {
        ImGui::PushID("CombatTuning");

        if (Sandbox_BeginPropertyTable("CombatProps"))
        {
            Sandbox_SliderFloat("Melee HP", "##MeleeHP", &w.cfg.meleeHp, 10.0f, 300.0f, "%.1f");
            Sandbox_SliderFloat("Melee DMG", "##MeleeDMG", &w.cfg.meleeDmg, 1.0f, 50.0f, "%.1f");
            Sandbox_SliderFloat("Caster HP", "##CasterHP", &w.cfg.casterHp, 10.0f, 200.0f, "%.1f");
            Sandbox_SliderFloat("Caster DMG", "##CasterDMG", &w.cfg.casterDmg, 1.0f, 60.0f, "%.1f");
            Sandbox_SliderFloat("Tower DMG", "##TowerDMG", &w.cfg.towerDmg, 10.0f, 200.0f, "%.1f");
            Sandbox_SliderFloat("Champ DMG", "##ChampDMG", &w.cfg.champDmg, 10.0f, 150.0f, "%.1f");
            Sandbox_EndPropertyTable();
        }

        ImGui::PopID();
    }

    // ------------------------------------------------------------------------
    // Scenarios
    // ------------------------------------------------------------------------
    if (Sandbox_BeginCard("Scenarios"))
    {
        ImGui::PushID("Scenarios");

        if (ImGui::Button("Neutral", ImVec2(half, 32)))
            Sandbox_LoadNeutral(w, accum, paused, sandbox.seed);
        ImGui::SameLine();
        if (ImGui::Button("Freeze", ImVec2(half, 32)))
            Sandbox_LoadFreeze(w, accum, paused, sandbox.seed);

        if (ImGui::Button("Slow Push", ImVec2(half, 32)))
            Sandbox_LoadSlowPush(w, accum, paused, sandbox.seed);
        ImGui::SameLine();
        if (ImGui::Button("Shove", ImVec2(half, 32)))
            Sandbox_LoadShove(w, accum, paused, sandbox.seed);

        if (ImGui::Button("Tower Aggro", ImVec2(half, 32)))
            Sandbox_LoadTowerAggro(w, accum, paused, sandbox.seed);

        ImGui::PopID();
    }

    // ------------------------------------------------------------------------
    // Stats
    // ------------------------------------------------------------------------
    Sandbox_DrawSectionHeader("Stats");

    if (ImGui::BeginTable("stats_table", 2,
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg))
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Tick");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", w.tick);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Blue wave No.");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", w.waves[TEAM_BLUE].number);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Red wave No.");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", w.waves[TEAM_RED].number);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Blue last hits");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", w.blueKills);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Red last hits");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", w.redKills);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Blue minions alive");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", CountAlive(w, KIND_MINION, TEAM_BLUE));

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Red minions alive");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", CountAlive(w, KIND_MINION, TEAM_RED));

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Projectiles alive");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", CountAlive(w, KIND_PROJECTILE, TEAM_NEUTRAL));

        ImGui::EndTable();
    }

    ImGui::End();
}