#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <functional>
class Window
{
public:
	Window() = default;
	~Window();
	bool Init(HINSTANCE hInstance, int width, int height, const std::wstring& title);
	void Show(int nCmdShow);
	void Destroy();
	HWND GetHWND() const { return m_hwnd; }
	int GetWidth() const { return m_width; }
	int GetHeight() const { return m_height; }
	void SetResizeCallback(std::function<void(int, int)> cb) { m_resizeCb = cb; }
private:
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	HWND m_hwnd = nullptr;
	int m_width = 0;
	int m_height = 0;
	std::wstring m_title;
	std::function<void(int, int)> m_resizeCb;
};