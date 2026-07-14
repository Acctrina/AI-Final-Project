#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <fstream>

#include "cprocessing.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include "MinHook.h"

#include "src/Sim.h"
#include "src/Renderer.h"
#include "src/Analysis.h"
#include "src/Sandbox.h"

// Provided by imgui_impl_win32.cpp; forward-declared here as the backend examples do.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Lane simulation plus the rendering/debug toggles driven from the keyboard.
static World         g_world;
static RenderOptions g_render = { true, false, false, true, true, true };
static bool          g_paused = false;
static float         g_accum  = 0.0f;

static SandboxState  g_sandbox;
static AnalysisState g_analysis; // read-only observer: influence field + technique detectors

// CProcessing owns the GLFW window and its message loop, and only exposes the HWND.
// We subclass the window procedure so ImGui sees input first, then chain to GLFW's proc.
static WNDPROC s_originalWndProc = nullptr;
static HWND    s_windowHandle    = nullptr;
static bool    s_imguiReady      = false;

// CProcessing draws through NanoVG, which flushes AFTER its post-update hook, so anything
// we draw in an update callback ends up underneath the scene. To land on top we hook the
// buffer swap (gdi32 SwapBuffers, which GLFW calls to present) and render ImGui right before
// the real swap runs - i.e. after CProcessing has flushed its frame.
typedef BOOL(WINAPI* SwapBuffers_t)(HDC);
static SwapBuffers_t s_originalSwapBuffers = nullptr;

static LRESULT CALLBACK ImGuiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return 1;
	return CallWindowProcW(s_originalWndProc, hWnd, msg, wParam, lParam);
}

static void imgui_render_frame(void)
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// Sandbox UI
	Sandbox_DrawImGui(g_world, g_render, g_sandbox, g_accum, g_paused);

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// Our SwapBuffers replacement: draw the overlay on the finished frame, then present as usual.
static BOOL WINAPI HookedSwapBuffers(HDC hdc)
{
	if (s_imguiReady)
		imgui_render_frame();
	return s_originalSwapBuffers(hdc);
}

static void imgui_ini_check(void) {
	std::ifstream ifs("imgui.ini");

	// If the file failed to open, it means there is no imgui.ini
	if (!ifs) {
		// Create the imgui ini
		std::ofstream ofs("imgui.ini", std::ios_base::binary | std::ios_base::out);
		// and then copy from assets
		ifs.open("Assets\\imgui.ini", std::ios_base::binary | std::ios_base::in);
		ofs << ifs.rdbuf();
	}
}

static void imgui_init(void)
{
	s_windowHandle = CP_System_GetWindowHandle();
	if (!s_windowHandle)
		return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	Sandbox_ApplyImGuiTheme();
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	// Intercept messages ahead of GLFW; keep the original proc to chain to.
	s_originalWndProc = reinterpret_cast<WNDPROC>(
		SetWindowLongPtrW(s_windowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ImGuiWndProc)));

	ImGui_ImplWin32_Init(s_windowHandle);
	ImGui_ImplOpenGL3_Init(nullptr); // nullptr = auto-detect the GLSL version from the GL context

	// Hook the present call so ImGui renders after CProcessing's NanoVG flush.
	if (MH_Initialize() == MH_OK)
	{
		if (MH_CreateHookApi(L"gdi32", "SwapBuffers",
				reinterpret_cast<LPVOID>(&HookedSwapBuffers),
				reinterpret_cast<LPVOID*>(&s_originalSwapBuffers)) == MH_OK)
		{
			MH_EnableHook(MH_ALL_HOOKS);
		}
	}

	s_imguiReady = true;
}

static void imgui_shutdown(void)
{
	if (!s_imguiReady)
		return;

	// Stop the overlay from rendering, then tear the hook down before ImGui goes away.
	s_imguiReady = false;
	MH_DisableHook(MH_ALL_HOOKS);
	MH_Uninitialize();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	// Restore GLFW's original window procedure.
	if (s_originalWndProc)
		SetWindowLongPtrW(s_windowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
}

void game_init(void)
{
	World_Init(g_world, 1337u);

	CP_System_SetWindowSize(g_world.cfg.windowWidth, g_world.cfg.windowHeight);
	CP_System_SetFrameRate(60.0f);

	CP_Font_Load("Assets/ShareTech-Regular.ttf");

	// Check if there is already an imgui.ini saved up
	imgui_ini_check();

	// Init ImGui after the final window size is set so we grab the current HWND.
	imgui_init();
}

void game_update(void)
{
	// Input Gating so that clicks on ImGui Windows does not leak into the game.
	ImGuiIO& io = ImGui::GetIO();
	const bool allowMouseToGame = !io.WantCaptureMouse;
	const bool allowKeyboardToGame = !io.WantCaptureKeyboard;


	// Champion order for this frame: a right-click either targets the enemy under the
	// cursor (attack-move) or the ground point (A* move). Consumed once, on the click.
	SimInput in;
	in.issued       = false;
	in.worldPoint   = CP_Vector_Zero();
	in.targetEntity = InvalidId();
	if (allowMouseToGame && CP_Input_MouseTriggered(MOUSE_BUTTON_RIGHT))
	{
		CP_Vector cursor = CP_Vector_Set(CP_Input_GetMouseX(), CP_Input_GetMouseY());
		in.issued       = true;
		in.worldPoint   = cursor;
		in.targetEntity = World_PickEnemyAt(g_world, cursor, TEAM_BLUE, 10.0f);
	}

	// Debug / flow toggles.
	if (CP_Input_KeyTriggered(KEY_SPACE)) g_paused = !g_paused;
	if (CP_Input_KeyTriggered(KEY_1))     g_render.showAggroLines = !g_render.showAggroLines;
	if (CP_Input_KeyTriggered(KEY_2))     g_render.showRanges = !g_render.showRanges;
	if (CP_Input_KeyTriggered(KEY_3))     g_render.showInfluence = !g_render.showInfluence;
	if (CP_Input_KeyTriggered(KEY_4))     g_render.showEquilibrium = !g_render.showEquilibrium;
	if (CP_Input_KeyTriggered(KEY_5))     g_render.showAttackBars = !g_render.showAttackBars;
	bool step = allowKeyboardToGame && CP_Input_KeyTriggered(KEY_PERIOD) || CP_Input_KeyTriggered(KEY_RIGHT);

	// Fixed-timestep advance: the sim only ever steps by cfg.fixedDt, so it stays
	// deterministic and reproducible regardless of the real frame rate.
	float dt = CP_System_GetDt();
	if (dt > g_world.cfg.maxFrameTime)
		dt = g_world.cfg.maxFrameTime;

	int steps = 0; // sim sub-steps taken this frame, to advance the analysis on sim time
	if (!g_paused)
	{
		g_accum += dt;
		while (g_accum >= g_world.cfg.fixedDt)
		{
			Sim_Tick(g_world, g_world.cfg.fixedDt, in);
			in.issued = false; // apply the click on the first sub-step only
			g_accum -= g_world.cfg.fixedDt;
			++steps;
		}
	}
	else if (step)
	{
		Sim_Tick(g_world, g_world.cfg.fixedDt, in);
		++steps;
	}

	// Analysis observes the world after it has stepped. Passing sim-elapsed (0 while
	// paused) keeps its detectors pause-aware and frame-rate independent.
	Analysis_Update(g_world, g_analysis, steps * g_world.cfg.fixedDt);

	g_render.paused = g_paused;
	Render_World(g_world, g_analysis, g_render);

	if (allowKeyboardToGame && CP_Input_KeyDown(KEY_Q))
		CP_Engine_Terminate();
}

void game_exit(void)
{
	imgui_shutdown();
}




int main(void)
{
	//CP_Engine_SetNextGameState(splash_screen_init, splash_screen_update, splash_screen_exit);
	CP_Engine_SetNextGameState(game_init, game_update, game_exit);
	CP_Engine_Run(true);
	return 0;
}
