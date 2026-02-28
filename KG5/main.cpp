#include "Window.h"
#include "Renderer.h"
#include "Timer.h"
#include "InputDevice.h"

#include <string>

static std::string GetExeDir()
{
	char buf[MAX_PATH]{};
	GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string path(buf);
	size_t p = path.find_last_of("\\/");
	return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

static bool TryLoadObjWithFallbacks(Renderer& r, const std::string& exeDir)
{
	// Visual Studio usually runs the exe from: <project>\\x64\\Debug\\ 
	// while assets are often in: <project>\\textures\\ 
	const char* candidates[] = {
	"textures/broodmother.obj",
		"..\\textures\\broodmother.obj",
		"..\\..\\textures\\broodmother.obj",
		"..\\..\\..\\textures\\broodmother.obj",
};
for (const char* rel : candidates)
{
	if (r.LoadObj(exeDir + rel))
		return true;
}
return false;
}

class App
{
public:
	bool Init(HINSTANCE hInstance)
	{
		if (!m_window.Init(hInstance, 1280, 720, L"broodmother"))
			return false;
		m_window.SetResizeCallback([this](int w, int h) {
			m_renderer.OnResize(w, h);
			});
		if (!m_renderer.Init(m_window.GetHWND(),
			m_window.GetWidth(),
			m_window.GetHeight()))
			return false;
		// Texture tiling 2x2, slow horizontal UV scroll
		m_renderer.SetTexTiling(2.0f, 2.0f);
		m_renderer.SetTexScroll(0.05f, 0.0f);
		// Load an OBJ model (tries several common locations relative to the .exe)
		std::string exeDir = GetExeDir();
		if (!TryLoadObjWithFallbacks(m_renderer, exeDir))
		{
			std::wstring msg =
				L"Failed to load broodmother.obj.\n\n"
				L"Fix options:\n"
				L"1) Copy your 'textures' folder next to KG5.exe (x64\\Debug\\textures\\...)\n"
				L"2) Or keep assets in the project root (KG5\\textures\\...), the app will find it\n\n"
				L"Exe dir:\n";
			msg += std::wstring(exeDir.begin(), exeDir.end());
			MessageBox(nullptr, msg.c_str(), L"Error", MB_OK | MB_ICONERROR);
			return false;
		}
		m_timer.Reset();
		return true;
	}
	void Show(int nCmdShow) { m_window.Show(nCmdShow); }
	int Run()
	{
		MSG msg{};
		while (true)
		{
			while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
					return (int)msg.wParam;
				switch (msg.message)
				{
				case WM_KEYDOWN: m_input.OnKeyDown(msg.wParam); break;
				case WM_KEYUP: m_input.OnKeyUp(msg.wParam); break;
				case WM_MOUSEMOVE: m_input.OnMouseMove(LOWORD(msg.lParam), HIWORD(msg.lParam)); break;
				case WM_LBUTTONDOWN: m_input.OnMouseDown(0); break;
				case WM_LBUTTONUP: m_input.OnMouseUp(0); break;
				case WM_RBUTTONDOWN: m_input.OnMouseDown(1); break;
				case WM_RBUTTONUP: m_input.OnMouseUp(1); break;
				}
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			m_timer.Tick();
			const float clear[] = { 0.1f, 0.1f, 0.15f, 1.0f };
			m_renderer.BeginFrame(clear);
			m_renderer.DrawScene(m_timer.TotalTime(), m_timer.DeltaTime());
			m_renderer.EndFrame();
			m_input.EndFrame();
		}
	}
private:
	Window m_window;
	Renderer m_renderer;
	Timer m_timer;
	InputDevice m_input;
};
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
	App app;
	if (!app.Init(hInstance))
	{
		MessageBox(nullptr, L"Init failed!", L"Error", MB_OK | MB_ICONERROR);
		return -1;
	}
	app.Show(nCmdShow);
	return app.Run();
}