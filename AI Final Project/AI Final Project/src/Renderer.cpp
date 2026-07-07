#include "Renderer.h"

#include <cstdio>
#include <cmath>

// All CProcessing drawing lives here. The renderer only reads the World; it must
// never change simulation state. Debug overlays make the AI's decisions visible:
// per-agent state colour, a line to the current aggro target, and range rings.

static CP_Color TeamFill(Team team)
{
	if (team == TEAM_BLUE) return CP_Color_Create(70, 130, 220, 255);
	if (team == TEAM_RED)  return CP_Color_Create(220, 80, 70, 255);
	return CP_Color_Create(160, 160, 160, 255);
}

// FSM state -> outline colour, so a glance reads what every minion is "thinking".
static CP_Color StateStroke(FsmState s)
{
	switch (s)
	{
	case STATE_MARCHING:  return CP_Color_Create(235, 235, 235, 255); // white
	case STATE_IN_COMBAT: return CP_Color_Create(245, 200, 60, 255);  // amber
	case STATE_THREATENED:return CP_Color_Create(255, 90, 90, 255);   // red
	}
	return CP_Color_Create(235, 235, 235, 255);
}

static void DrawHealthBar(const Entity& e)
{
	if (e.hp >= e.maxHp)
		return;
	float w = e.radius * 2.4f;
	float h = 5.0f;
	float x = e.pos.x - w * 0.5f;
	float y = e.pos.y - e.radius - 11.0f;
	float frac = e.hp / e.maxHp;
	if (frac < 0.0f) frac = 0.0f;

	CP_Settings_NoStroke();
	CP_Settings_Fill(CP_Color_Create(30, 30, 30, 220));
	CP_Graphics_DrawRectAdvanced(x, y, w, h, 0.0f, 0.0f);
	CP_Settings_Fill(CP_Color_Create(90, 220, 90, 255));
	CP_Graphics_DrawRectAdvanced(x, y, w * frac, h, 0.0f, 0.0f);
}

static void DrawRing(CP_Vector c, float radius, CP_Color col)
{
	CP_Settings_NoFill();
	CP_Settings_Stroke(col);
	CP_Settings_StrokeWeight(1.0f);
	CP_Graphics_DrawCircle(c.x, c.y, radius * 2.0f); // CProcessing takes a diameter
}

void Render_World(World& w, const AnalysisState& analysis, const RenderOptions& opt)
{
	CP_Settings_RectMode(CP_POSITION_CORNER);

	// Background.
	CP_Graphics_ClearBackground(CP_Color_Create(20, 22, 26, 255));

	// Lane band as a filled quad (a thick line would be clamped by the renderer's
	// max stroke width, so widening the lane wouldn't show). Build the four corners
	// from the lane axis and its perpendicular.
	CP_Vector dir  = Lane_Dir(w.lane);
	CP_Vector perp = V(-dir.y, dir.x);
	float     half = w.lane.width * 0.5f;
	CP_Vector a = VAdd(w.lane.blueBase, VScale(perp,  half));
	CP_Vector b = VAdd(w.lane.blueBase, VScale(perp, -half));
	CP_Vector c = VAdd(w.lane.redBase,  VScale(perp, -half));
	CP_Vector d = VAdd(w.lane.redBase,  VScale(perp,  half));
	CP_Settings_NoStroke();
	CP_Settings_Fill(CP_Color_Create(38, 42, 48, 255));
	CP_Graphics_DrawQuad(a.x, a.y, b.x, b.y, c.x, c.y, d.x, d.y);

	// Lighter centre stripe (thin enough to stay under the stroke-width cap).
	CP_Settings_NoFill();
	CP_Settings_Stroke(CP_Color_Create(52, 57, 64, 255));
	CP_Settings_StrokeWeight(2.0f);
	CP_Graphics_DrawLine(w.lane.blueBase.x, w.lane.blueBase.y, w.lane.redBase.x, w.lane.redBase.y);

	// Influence heatmap: one tinted slice per lane column, blue where blue presence
	// dominates and red where red does, opacity tracking the margin. This is the
	// analysis layer's raw signal - the equilibrium marker below is just where it
	// crosses zero.
	if (opt.showInfluence && analysis.cols > 0)
	{
		CP_Settings_NoStroke();
		for (int col = 0; col < analysis.cols; ++col)
		{
			float net = analysis.netInfluence[col];
			float mag = std::fabs(net) / analysis.maxAbsInfluence; // 0..1
			if (mag < 0.02f)
				continue;
			float t0 = col / (float)analysis.cols;
			float t1 = (col + 1) / (float)analysis.cols;
			CP_Vector q0 = Lane_PointAt(w.lane, t0);
			CP_Vector q1 = Lane_PointAt(w.lane, t1);
			CP_Vector s0 = VAdd(q0, VScale(perp,  half));
			CP_Vector s1 = VAdd(q0, VScale(perp, -half));
			CP_Vector s2 = VAdd(q1, VScale(perp, -half));
			CP_Vector s3 = VAdd(q1, VScale(perp,  half));
			unsigned char al = (unsigned char)(mag * 120.0f);
			CP_Color col4 = (net >= 0.0f) ? CP_Color_Create(70, 130, 220, al)
			                              : CP_Color_Create(220, 80, 70, al);
			CP_Settings_Fill(col4);
			CP_Graphics_DrawQuad(s0.x, s0.y, s1.x, s1.y, s2.x, s2.y, s3.x, s3.y);
		}
	}

	// Nexuses at the lane ends.
	CP_Settings_NoStroke();
	CP_Settings_Fill(TeamFill(TEAM_BLUE));
	CP_Graphics_DrawCircle(w.lane.blueBase.x, w.lane.blueBase.y, 150.0f);
	CP_Settings_Fill(TeamFill(TEAM_RED));
	CP_Graphics_DrawCircle(w.lane.redBase.x, w.lane.redBase.y, 150.0f);

	// Aggro lines first, so entities draw on top of them.
	if (opt.showAggroLines)
	{
		CP_Settings_StrokeWeight(1.0f);
		for (size_t i = 0; i < w.ents.size(); ++i)
		{
			Entity& e = w.ents[i];
			if (!e.alive || e.kind == KIND_PROJECTILE || !IsValidId(e.target))
				continue;
			Entity* t = World_Get(w, e.target);
			if (!t)
				continue;
			CP_Settings_Stroke(CP_Color_Create(255, 235, 120, 130));
			CP_Graphics_DrawLine(e.pos.x, e.pos.y, t->pos.x, t->pos.y);
		}
	}

	// Entities.
	for (size_t i = 0; i < w.ents.size(); ++i)
	{
		Entity& e = w.ents[i];
		if (!e.alive)
			continue;

		if (e.kind == KIND_PROJECTILE)
		{
			CP_Settings_NoStroke();
			CP_Settings_Fill(TeamFill(e.team));
			CP_Graphics_DrawCircle(e.pos.x, e.pos.y, e.radius * 2.0f);
			continue;
		}

		if (opt.showRanges)
		{
			if (e.kind == KIND_TOWER)
				DrawRing(e.pos, e.attackRange, CP_Color_Create(255, 120, 120, 90));
			else if (e.kind == KIND_MINION)
				DrawRing(e.pos, w.cfg.detectRange, CP_Color_Create(120, 160, 255, 50));
			else if (e.kind == KIND_CHAMPION)
				DrawRing(e.pos, e.attackRange, CP_Color_Create(120, 255, 160, 90));
		}

		CP_Settings_Fill(TeamFill(e.team));
		if (e.kind == KIND_TOWER)
		{
			CP_Settings_Stroke(CP_Color_Create(235, 235, 235, 255));
			CP_Settings_StrokeWeight(2.0f);
			float s = e.radius * 2.0f;
			CP_Graphics_DrawRectAdvanced(e.pos.x - e.radius, e.pos.y - e.radius, s, s, 0.0f, 3.0f);
			DrawHealthBar(e);
			continue;
		}

		if (e.kind == KIND_CHAMPION)
		{
			CP_Settings_Stroke(CP_Color_Create(120, 255, 160, 255));
			CP_Settings_StrokeWeight(3.0f);
			CP_Graphics_DrawCircle(e.pos.x, e.pos.y, e.radius * 2.0f);
			DrawHealthBar(e);
			continue;
		}

		// Minion: fill by team, outline by FSM state; cannon gets an inner dot.
		CP_Settings_Stroke(StateStroke(e.state));
		CP_Settings_StrokeWeight(2.0f);
		CP_Graphics_DrawCircle(e.pos.x, e.pos.y, e.radius * 2.0f);
		if (e.minionType == MINION_CASTER)
		{
			CP_Settings_NoStroke();
			CP_Settings_Fill(CP_Color_Create(255, 255, 255, 200));
			CP_Graphics_DrawCircle(e.pos.x, e.pos.y, e.radius * 0.7f);
		}
		else if (e.minionType == MINION_CANNON)
		{
			CP_Settings_NoStroke();
			CP_Settings_Fill(CP_Color_Create(20, 20, 20, 220));
			CP_Graphics_DrawCircle(e.pos.x, e.pos.y, e.radius * 0.8f);
		}
		DrawHealthBar(e);
	}

	// Champion move order: draw the A* path and the destination marker.
	if (w.champHasGoal && !w.champPath.empty())
	{
		Entity* champ = World_Get(w, w.champion);
		CP_Vector prev = champ ? champ->pos : w.champPath[0];
		CP_Settings_Stroke(CP_Color_Create(120, 255, 160, 150));
		CP_Settings_StrokeWeight(2.0f);
		for (size_t i = w.champPathIdx; i < w.champPath.size(); ++i)
		{
			CP_Graphics_DrawLine(prev.x, prev.y, w.champPath[i].x, w.champPath[i].y);
			prev = w.champPath[i];
		}
		CP_Settings_NoStroke();
		CP_Settings_Fill(CP_Color_Create(120, 255, 160, 190));
		CP_Graphics_DrawCircle(w.champGoal.x, w.champGoal.y, 14.0f);
	}

	// HUD.
	char buf[160];
	CP_Settings_Fill(CP_Color_Create(240, 240, 240, 255));
	CP_Settings_TextSize(22.0f);
	sprintf_s(buf, sizeof(buf), "Wave B:%d  R:%d    Last-hits  Blue:%d  Red:%d    tick:%lld%s",
	          w.waves[TEAM_BLUE].number, w.waves[TEAM_RED].number,
	          w.blueKills, w.redKills, w.tick, opt.paused ? "   [PAUSED]" : "");
	CP_Font_DrawText(buf, 18.0f, 32.0f);

	CP_Settings_TextSize(15.0f);
	CP_Settings_Fill(CP_Color_Create(170, 170, 170, 255));
	CP_Font_DrawText("Right-click: move / attack   [Space] pause  [.] step  [1] aggro  [2] ranges  [3] influence  [4] equilibrium  Q quit",
	                 18.0f, (float)w.cfg.windowHeight - 22.0f);

	// --- Analysis overlays: equilibrium marker + recognised-technique banner ----
	// The equilibrium is where the influence field crosses zero (the wave's meeting
	// point); the short tick shows which way it is drifting - i.e. who is pushing.
	if (opt.showEquilibrium && analysis.equilibriumValid)
	{
		CP_Vector pe  = Lane_PointAt(w.lane, analysis.equilibriumT);
		CP_Vector top = VAdd(pe, VScale(perp,  half));
		CP_Vector bot = VAdd(pe, VScale(perp, -half));

		CP_Settings_Stroke(CP_Color_Create(255, 240, 120, 220));
		CP_Settings_StrokeWeight(3.0f);
		CP_Graphics_DrawLine(top.x, top.y, bot.x, bot.y);

		// Drift tick along the lane axis: length grows with the push speed.
		if (std::fabs(analysis.equilibriumVel) > 0.0005f)
		{
			CP_Vector ld  = Lane_Dir(w.lane);
			float     sgn = analysis.equilibriumVel > 0.0f ? 1.0f : -1.0f;
			float     len = 30.0f + std::fabs(analysis.equilibriumVel) * 260.0f;
			CP_Vector tip = VAdd(pe, VScale(ld, sgn * len));
			CP_Graphics_DrawLine(pe.x, pe.y, tip.x, tip.y);
		}

		CP_Settings_NoStroke();
		CP_Settings_Fill(CP_Color_Create(255, 240, 120, 235));
		CP_Graphics_DrawCircle(pe.x, pe.y, 13.0f);

		CP_Settings_TextAlignment(CP_TEXT_ALIGN_H_CENTER, CP_TEXT_ALIGN_V_MIDDLE);
		CP_Settings_TextSize(13.0f);
		CP_Settings_Fill(CP_Color_Create(255, 240, 120, 255));
		CP_Font_DrawText("EQUILIBRIUM", top.x, top.y - 12.0f);
	}

	// Technique banner: shown whenever a detector has fired recently, fading out over
	// its lifetime. This is the thesis on screen - a simple rule set producing a named,
	// skill-expressive play.
	if (analysis.banner.ttl > 0.0f && analysis.banner.tech != TECH_NONE)
	{
		float a01 = analysis.banner.ttl / w.cfg.bannerTtl;
		if (a01 > 1.0f) a01 = 1.0f;
		if (a01 < 0.0f) a01 = 0.0f;
		unsigned char al = (unsigned char)(a01 * 255.0f);
		float cx = w.cfg.windowWidth * 0.5f;

		CP_Settings_TextAlignment(CP_TEXT_ALIGN_H_CENTER, CP_TEXT_ALIGN_V_MIDDLE);
		CP_Settings_Fill(CP_Color_Create(245, 245, 250, al));
		CP_Settings_TextSize(46.0f);
		CP_Font_DrawText(Technique_Name(analysis.banner.tech), cx, 92.0f);

		CP_Color who = (analysis.banner.byTeam == TEAM_BLUE)
		             ? CP_Color_Create(120, 180, 255, al)
		             : CP_Color_Create(255, 120, 110, al);
		CP_Settings_Fill(who);
		CP_Settings_TextSize(18.0f);
		CP_Font_DrawText(analysis.banner.byTeam == TEAM_BLUE ? "BLUE executed" : "RED executed",
		                 cx, 126.0f);
	}

	// Leave text alignment as the HUD expects it for the next frame.
	CP_Settings_TextAlignment(CP_TEXT_ALIGN_H_LEFT, CP_TEXT_ALIGN_V_BASELINE);
}
