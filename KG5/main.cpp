#include "Window.h"
#include "RenderingSystem.h"
#include "Timer.h"
#include "InputDevice.h"
#include <windowsx.h>
#include <cstring>
#include <stdexcept>
#include <string>

class App
{
public:
    bool Init(HINSTANCE hInstance)
    {
        if (!m_window.Init(hInstance, 1280, 720, L"Deferred Renderer - Dirty Scene"))
            return false;

        if (!m_renderer.Init(m_window.GetHWND(),
            m_window.GetWidth(),
            m_window.GetHeight()))
            return false;

        m_window.SetResizeCallback([this](int w, int h) {
            m_renderer.OnResize(w, h);
            });

        m_renderer.SetTexTiling(1.0f, 1.0f);
        m_renderer.SetTexScroll(0.0f, 0.0f);

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
                    m_input.OnMouseMove(GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam));
                    m_renderer.OnMouseMove(GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam));
                    break;
                case WM_LBUTTONDOWN:
                    m_input.OnMouseDown(0);
                    m_renderer.OnMouseDown(GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam));
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
    try
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
    catch (const std::exception& ex)
    {
        std::wstring msg = L"Unhandled exception: ";
        msg += std::wstring(ex.what(), ex.what() + std::strlen(ex.what()));
        MessageBox(nullptr, msg.c_str(), L"Runtime Error", MB_OK | MB_ICONERROR);
        return -1;
    }
}
