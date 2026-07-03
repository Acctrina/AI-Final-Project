#include "Nav.h"
#include "Sim.h"

#include <queue>
#include <vector>
#include <utility>
#include <functional>
#include <cmath>
#include <cfloat>

// ---------------------------------------------------------------------------
// Grid helpers.
// ---------------------------------------------------------------------------
static inline int  Idx(const NavGrid& n, int cx, int cy) { return cy * n.cols + cx; }
static inline bool InBounds(const NavGrid& n, int cx, int cy)
{
	return cx >= 0 && cy >= 0 && cx < n.cols && cy < n.rows;
}
static inline bool Walkable(const NavGrid& n, int cx, int cy)
{
	return InBounds(n, cx, cy) && !n.blocked[Idx(n, cx, cy)];
}
static void CellOf(const NavGrid& n, CP_Vector p, int& cx, int& cy)
{
	cx = (int)((p.x - n.originX) / n.cellSize);
	cy = (int)((p.y - n.originY) / n.cellSize);
}
static CP_Vector CellCenter(const NavGrid& n, int cx, int cy)
{
	return V(n.originX + (cx + 0.5f) * n.cellSize, n.originY + (cy + 0.5f) * n.cellSize);
}

// Nearest walkable cell to a world point (linear scan; only called a few times).
static bool NearestWalkable(const NavGrid& n, CP_Vector p, int& outX, int& outY)
{
	int px, py;
	CellOf(n, p, px, py);
	if (Walkable(n, px, py)) { outX = px; outY = py; return true; }

	float best = FLT_MAX;
	bool  found = false;
	for (int cy = 0; cy < n.rows; ++cy)
		for (int cx = 0; cx < n.cols; ++cx)
		{
			if (n.blocked[Idx(n, cx, cy)])
				continue;
			float dx = (float)(cx - px), dy = (float)(cy - py);
			float d = dx * dx + dy * dy;
			if (d < best) { best = d; outX = cx; outY = cy; found = true; }
		}
	return found;
}

// 8-connected neighbour offsets and step costs.
static const int   kNX[8]    = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int   kNY[8]    = { 0, 0, 1, -1, 1, -1, 1, -1 };
static const float kNCost[8] = { 1, 1, 1, 1, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f };

// A diagonal step is only legal if both orthogonal cells beside it are open, so units
// never clip through a tower corner.
static bool DiagOK(const NavGrid& n, int cx, int cy, int k)
{
	if (k < 4) return true;
	return Walkable(n, cx + kNX[k], cy) && Walkable(n, cx, cy + kNY[k]);
}

// ---------------------------------------------------------------------------
// Build the static grid.
// ---------------------------------------------------------------------------
void Nav_Build(const World& w, NavGrid& nav)
{
	nav.cellSize = 24.0f;
	nav.originX  = 0.0f;
	nav.originY  = 0.0f;
	nav.cols = (int)ceilf(w.cfg.windowWidth  / nav.cellSize);
	nav.rows = (int)ceilf(w.cfg.windowHeight / nav.cellSize);
	int total = nav.cols * nav.rows;
	nav.blocked.assign(total, 0);

	float     half   = w.lane.width * 0.5f;
	CP_Vector dir    = Lane_Dir(w.lane);
	CP_Vector perp   = V(-dir.y, dir.x);
	float     laneLen = VDist(w.lane.blueBase, w.lane.redBase);

	for (int cy = 0; cy < nav.rows; ++cy)
		for (int cx = 0; cx < nav.cols; ++cx)
		{
			CP_Vector c   = CellCenter(nav, cx, cy);
			CP_Vector rel = VSub(c, w.lane.blueBase);
			float along   = rel.x * dir.x + rel.y * dir.y;
			float side    = rel.x * perp.x + rel.y * perp.y;
			float aside   = side < 0.0f ? -side : side;

			bool inBand = (aside <= half - 6.0f) && (along >= -10.0f) && (along <= laneLen + 10.0f);
			if (!inBand)
			{
				nav.blocked[Idx(nav, cx, cy)] = 1;
				continue;
			}

			// Towers are static blockers; leave a minion-radius of margin around them.
			for (size_t i = 0; i < w.ents.size(); ++i)
			{
				const Entity& e = w.ents[i];
				if (e.alive && e.kind == KIND_TOWER && VDist(c, e.pos) <= e.radius + 20.0f)
				{
					nav.blocked[Idx(nav, cx, cy)] = 1;
					break;
				}
			}
		}
}

// ---------------------------------------------------------------------------
// A* for the champion.
// ---------------------------------------------------------------------------
static float Octile(int ax, int ay, int bx, int by)
{
	int dx = ax > bx ? ax - bx : bx - ax;
	int dy = ay > by ? ay - by : by - ay;
	int lo = dx < dy ? dx : dy;
	int hi = dx < dy ? dy : dx;
	return (hi - lo) + 1.41421356f * lo;
}

bool Nav_FindPath(const NavGrid& nav, CP_Vector start, CP_Vector goal, std::vector<CP_Vector>& out)
{
	out.clear();

	int sx, sy, gx, gy;
	if (!NearestWalkable(nav, start, sx, sy) || !NearestWalkable(nav, goal, gx, gy))
		return false;
	int startIdx = Idx(nav, sx, sy);
	int goalIdx  = Idx(nav, gx, gy);
	if (startIdx == goalIdx)
	{
		out.push_back(goal);
		return true;
	}

	int total = nav.cols * nav.rows;
	std::vector<float> g(total, FLT_MAX);
	std::vector<int>   came(total, -1);

	typedef std::pair<float, int> Node; // (fScore, cellIndex)
	std::priority_queue<Node, std::vector<Node>, std::greater<Node> > pq;
	g[startIdx] = 0.0f;
	pq.push(Node(Octile(sx, sy, gx, gy), startIdx));

	bool found = false;
	while (!pq.empty())
	{
		int idx = pq.top().second;
		pq.pop();
		if (idx == goalIdx) { found = true; break; }
		int cx = idx % nav.cols;
		int cy = idx / nav.cols;
		for (int k = 0; k < 8; ++k)
		{
			int nx = cx + kNX[k];
			int ny = cy + kNY[k];
			if (!Walkable(nav, nx, ny) || !DiagOK(nav, cx, cy, k))
				continue;
			int   ni = Idx(nav, nx, ny);
			float ng = g[idx] + kNCost[k];
			if (ng < g[ni])
			{
				g[ni] = ng;
				came[ni] = idx;
				pq.push(Node(ng + Octile(nx, ny, gx, gy), ni));
			}
		}
	}

	if (!found)
		return false;

	// Walk the parent chain back, then reverse into forward order.
	std::vector<int> rev;
	for (int cur = goalIdx; cur != -1; cur = came[cur])
	{
		rev.push_back(cur);
		if (cur == startIdx)
			break;
	}
	for (size_t i = rev.size(); i-- > 0; )
	{
		int cur = rev[i];
		out.push_back(CellCenter(nav, cur % nav.cols, cur / nav.cols));
	}
	if (!out.empty())
		out.back() = goal; // finish exactly on the clicked point
	return true;
}
