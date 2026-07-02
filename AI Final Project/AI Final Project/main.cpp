#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "cprocessing.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include "MinHook.h"

// Provided by imgui_impl_win32.cpp; forward-declared here as the backend examples do.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

CP_Image logo;

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

	// --- UI goes here; demo window for now to confirm the integration works ---
	ImGui::ShowDemoWindow();

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

static void imgui_init(void)
{
	s_windowHandle = CP_System_GetWindowHandle();
	if (!s_windowHandle)
		return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

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
	logo = CP_Image_Load("Assets/DigiPen_Singapore_WEB_RED.png");
	CP_Settings_ImageMode(CP_POSITION_CORNER);
	CP_Settings_ImageWrapMode(CP_IMAGE_WRAP_CLAMP);

	CP_System_SetWindowSize(CP_Image_GetWidth(logo), CP_Image_GetHeight(logo));

	// Init ImGui after the final window size is set so we grab the current HWND.
	imgui_init();
}

void game_update(void)
{
	CP_Graphics_ClearBackground(CP_Color_Create(0, 0, 0, 255));
	CP_Image_Draw(logo, 0.f, 0.f, CP_Image_GetWidth(logo), CP_Image_GetHeight(logo), 255);
	if (CP_Input_KeyDown(KEY_Q))
	{
		CP_Engine_Terminate();
	}

}

void game_exit(void)
{
	imgui_shutdown();
	CP_Image_Free(&logo);
}




int main(void)
{
	//CP_Engine_SetNextGameState(splash_screen_init, splash_screen_update, splash_screen_exit);
	CP_Engine_SetNextGameState(game_init, game_update, game_exit);
	CP_Engine_Run(true);
	return 0;
}
