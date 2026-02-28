#pragma once
#include <Windows.h>
// Part Two: Timer framework class
class Timer
{
public:
	Timer();
	void Reset(); // Call before main loop
	void Tick(); // Call each frame
	float DeltaTime() const { return m_deltaTime; }
	float TotalTime() const;
private:
	double m_secondsPerCount = 0.0;
	double m_deltaTime = 0.0;
	__int64 m_baseTime = 0;
	__int64 m_pausedTime = 0;
	__int64 m_stopTime = 0;
	__int64 m_prevTime = 0;
	__int64 m_currTime = 0;
	bool m_stopped = false;
};