#include "Timer.h"
Timer::Timer()
{
	__int64 countsPerSec = 0;
	QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&countsPerSec));
	m_secondsPerCount = 1.0 / static_cast<double>(countsPerSec);
}
void Timer::Reset()
{
	__int64 currTime = 0;
	QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currTime));
	m_baseTime = currTime;
	m_prevTime = currTime;
	m_stopTime = 0;
	m_stopped = false;
}
void Timer::Tick()
{
	if (m_stopped)
	{
		m_deltaTime = 0.0;
		return;
	}
	QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&m_currTime));
	m_deltaTime = (m_currTime - m_prevTime) * m_secondsPerCount;
	m_prevTime = m_currTime;
	// Clamp to avoid large spikes (e.g. breakpoint hits)
	if (m_deltaTime < 0.0)
		m_deltaTime = 0.0;
}
float Timer::TotalTime() const
{
	if (m_stopped)
		return static_cast<float>((m_stopTime - m_pausedTime - m_baseTime) * m_secondsPerCount);
	return static_cast<float>((m_currTime - m_pausedTime - m_baseTime) * m_secondsPerCount);
}