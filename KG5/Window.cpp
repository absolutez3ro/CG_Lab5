#include "Window.h"
LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	Window* pWnd = nullptr;
	if (msg == WM_NCCREATE)
	{
		auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
		pWnd = reinterpret_cast<Window*>(cs->lpCreateParams);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWnd));
		pWnd->m_hwnd = hwnd;
	}
	else
	{
		pWnd = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	}
	if (pWnd)
	{
		switch (msg)
		{
		case WM_SIZE:
		{
			int w = LOWORD(lParam);
			int h = HIWORD(lParam);
			pWnd->m_width = w;
			pWnd->m_height = h;
			if (pWnd->m_resizeCb && w > 0 && h > 0)
				pWnd->m_resizeCb(w, h);
			return 0;
		}
		case WM_KEYDOWN:
			if (wParam == VK_ESCAPE)
				PostQuitMessage(0);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}
bool Window::Init(HINSTANCE hInstance, int width, int height, const std::wstring& title)
{
	m_width = width;
	m_height = height;
	m_title = title;
	WNDCLASSEX wc{};
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"DX12WindowClass";
	if (!RegisterClassEx(&wc)) return false;
	RECT rect = { 0, 0, width, height };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	m_hwnd = CreateWindowEx(
		0, L"DX12WindowClass", title.c_str(), WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		rect.right - rect.left, rect.bottom - rect.top,
		nullptr, nullptr, hInstance, this);
	return m_hwnd != nullptr;
}
void Window::Show(int nCmdShow)
{
	ShowWindow(m_hwnd, nCmdShow);
	UpdateWindow(m_hwnd);
}
void Window::Destroy()
{
	if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
}
Window::~Window() { Destroy(); }