#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <array>
// Part One: Refactored InputDevice
class InputDevice
{
public:
	// Call from WM_KEYDOWN / WM_KEYUP
	void OnKeyDown(WPARAM key) { if (key < 256) m_keys[key] = true; }
	void OnKeyUp(WPARAM key) { if (key < 256) m_keys[key] = false; }
	// Call from WM_MOUSEMOVE
	void OnMouseMove(int x, int y) { m_mouseX = x; m_mouseY = y; }
	void OnMouseDown(int btn) { if (btn < 3) m_mouseButtons[btn] = true; }
	void OnMouseUp(int btn) { if (btn < 3) m_mouseButtons[btn] = false; }
	bool IsKeyDown(UINT key) const { return key < 256 && m_keys[key]; }
	int MouseX() const { return m_mouseX; }
	int MouseY() const { return m_mouseY; }
	bool IsMouseDown(int btn) const { return btn < 3 && m_mouseButtons[btn]; }
	// Mouse delta (call EndFrame each frame)
	int MouseDX() const { return m_mouseDX; }
	int MouseDY() const { return m_mouseDY; }
	void EndFrame()
	{
		m_mouseDX = m_mouseX - m_prevMouseX;
		m_mouseDY = m_mouseY - m_prevMouseY;
		m_prevMouseX = m_mouseX;
		m_prevMouseY = m_mouseY;
	}
private:
	std::array<bool, 256> m_keys{};
	std::array<bool, 3> m_mouseButtons{};
	int m_mouseX = 0, m_mouseY = 0;
	int m_prevMouseX = 0, m_prevMouseY = 0;
	int m_mouseDX = 0, m_mouseDY = 0;
};