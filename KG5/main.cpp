#include "Window.h"
#include "RenderingSystem.h"
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

static bool TryLoadObjWithFallbacks(RenderingSystem& r, const std::string& exeDir)
{
    const char* candidates[] = {
        "assets/sponza/sponza.obj",
        "..\\assets\\sponza\\sponza.obj",
        "..\\..\\assets\\sponza\\sponza.obj",
        "..\\..\\..\\assets\\sponza\\sponza.obj",
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
        if (!m_window.Init(hInstance, 1280, 720, L"Sponza Deferred Renderer"))
            return false;

        m_window.SetResizeCallback([this](int w, int h) {
            m_renderer.OnResize(w, h);
            });

        if (!m_renderer.Init(m_window.GetHWND(),
            m_window.GetWidth(),
            m_window.GetHeight()))
            return false;

        m_renderer.SetTexTiling(1.0f, 1.0f);
        m_renderer.SetTexScroll(0.0f, 0.0f);

        std::string exeDir = GetExeDir();
        if (!TryLoadObjWithFallbacks(m_renderer, exeDir))
        {
            std::wstring msg =
                L"Failed to load sponza.obj.\n\n"
                L"Expected path example:\n"
                L"assets\\sponza\\sponza.obj\n\n"
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
                case WM_KEYDOWN:
                    m_input.OnKeyDown(msg.wParam);
                    m_renderer.OnKeyDown(msg.wParam);
                    break;
                case WM_KEYUP:
                    m_input.OnKeyUp(msg.wParam);
                    m_renderer.OnKeyUp(msg.wParam);
                    break;
                case WM_MOUSEMOVE:
                    m_input.OnMouseMove(LOWORD(msg.lParam), HIWORD(msg.lParam));
                    m_renderer.OnMouseMove(LOWORD(msg.lParam), HIWORD(msg.lParam));
                    break;
                case WM_LBUTTONDOWN:
                    m_input.OnMouseDown(0);
                    m_renderer.OnMouseDown(LOWORD(msg.lParam), HIWORD(msg.lParam));
                    break;
                case WM_LBUTTONUP:
                    m_input.OnMouseUp(0);
                    m_renderer.OnMouseUp();
                    break;
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
    RenderingSystem m_renderer;
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
