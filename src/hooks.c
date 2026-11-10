// winmm exports implemented in C instead of a plain jmp stub (see HOOKED in
// tools/gen_winmm_proxy.py). timeGetTime is called by the game's main loop every frame, which
// gives us a main-thread tick without patching iw4sp.exe.
#define WIN32_LEAN_AND_MEAN // keep mmsystem.h's dllimport prototypes out
#include <windows.h>
#include <stdio.h>

#include <winmm_names.h>

#include "log.h"

extern void *winmm_real[];

// [native] clocklog=1 (2026-11-02): how the game's clock behaves. Once a second: the calls of timeGetTime, how many different values they returned, the steps between
// the values and the wall clock (QueryPerformanceCounter) over the same span, and the timer resolution of the process (NtQueryTimerResolution: 156250 = 15.6 ms,
// the Windows default; 10000 = 1 ms).
static int clocklog = -1;
static void clock_probe(DWORD t)
{
	static DWORD last_t, steps[64];
	static LONGLONG last_q, report_q;
	static unsigned calls, distinct, nsteps;
	static DWORD max_step, min_step = 0xffffffff;
	LARGE_INTEGER q, f;
	if (clocklog < 0) {
		char path[MAX_PATH];
		DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
		while (n > 0 && path[n - 1] != '\\')
			n--;
		path[n] = 0;
		lstrcatA(path, "mw2coop\\settings.ini");
		clocklog = GetPrivateProfileIntA("native", "clocklog", 0, path) != 0;
	}
	QueryPerformanceCounter(&q);
	QueryPerformanceFrequency(&f);
	calls++;
	if (last_q) {
		if (t != last_t) {
			DWORD d = t - last_t;
			distinct++;
			if (d > max_step)
				max_step = d;
			if (d < min_step)
				min_step = d;
			if (nsteps < 64)
				steps[nsteps++] = d;
		}
	}
	last_t = t;
	last_q = q.QuadPart;
	if (!report_q)
		report_q = q.QuadPart;
	if (q.QuadPart - report_q >= f.QuadPart) {
		ULONG max_res = 0, min_res = 0, cur_res = 0;
		LONG (NTAPI *query)(PULONG, PULONG, PULONG) = (LONG (NTAPI *)(PULONG, PULONG, PULONG))GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
		if (query)
			query(&min_res, &max_res, &cur_res);
		char list[400];
		int len = 0;
		for (unsigned i = 0; i < nsteps && i < 24 && len < 360; i++)
			len += snprintf(list + len, sizeof(list) - (size_t)len, "%lu ", (unsigned long)steps[i]);
		list[len] = 0;
		log_printf("clock: %u timeGetTime calls in %.1f ms wall, %u different values, step min %lu max %lu ms, timer resolution %.2f ms (best %.2f), first steps: %s", calls,
			   (double)(q.QuadPart - report_q) * 1000.0 / (double)f.QuadPart, distinct, (unsigned long)(min_step == 0xffffffff ? 0 : min_step), (unsigned long)max_step,
			   cur_res / 10000.0, max_res / 10000.0, list);
		report_q = q.QuadPart;
		calls = distinct = nsteps = 0;
		max_step = 0;
		min_step = 0xffffffff;
	}
}

// [native] timerres=1 (experiment): ask Windows for the finest timer resolution (0.5 ms) for this process; the game itself asks for 1 ms.
static void timer_resolution_once(void)
{
	static volatile LONG done;
	// timeGetTime is a hot WinMM entry point (the game calls it throughout the frame loop).
	// Once the one-time setting has been considered, a plain atomic load avoids a locked
	// compare-exchange on every timer read. Keep the interlocked operation only for the
	// first caller, so simultaneous first calls still perform the setup once.
	if (__atomic_load_n(&done, __ATOMIC_RELAXED) != 0)
		return;
	if (InterlockedCompareExchange(&done, 1, 0) != 0)
		return;
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	while (n > 0 && path[n - 1] != '\\')
		n--;
	path[n] = 0;
	lstrcatA(path, "mw2coop\\settings.ini");
	if (!GetPrivateProfileIntA("native", "timerres", 0, path))
		return;
	LONG (NTAPI *set)(ULONG, BOOLEAN, PULONG) = (LONG (NTAPI *)(ULONG, BOOLEAN, PULONG))GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution");
	ULONG cur = 0;
	LONG r = set ? set(5000, TRUE, &cur) : -1;
	log_printf("clock: NtSetTimerResolution(0.5 ms) -> %ld, now %.2f ms", (long)r, cur / 10000.0);
}

// [native] qpctime=1 (experiment): the game's whole clock is timeGetTime (Sys_Milliseconds = timeGetTime() - the first reading; Com_Frame, rva 0x1fbde0, hands the server and the
// client the whole milliseconds between two readings as the frame's time step). Windows updates that value in uneven ticks (steps of 1 to 4 ms were measured at a 1 ms timer
// resolution), so the step of a 6.5 ms frame is wrong by up to a third. Here the value comes from the performance counter instead: the same milliseconds, floored from the
// exact time.
static int qpctime = -1;
static LONGLONG qpc_start;
static DWORD qpc_real_start;
static LONGLONG qpc_freq;
static DWORD precise_time(DWORD real)
{
	if (qpctime < 0) {
		char path[MAX_PATH];
		DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
		while (n > 0 && path[n - 1] != '\\')
			n--;
		path[n] = 0;
		lstrcatA(path, "mw2coop\\settings.ini");
		LARGE_INTEGER q, f;
		QueryPerformanceCounter(&q);
		QueryPerformanceFrequency(&f);
		qpc_start = q.QuadPart;
		qpc_freq = f.QuadPart;
		qpc_real_start = real;
		qpctime = GetPrivateProfileIntA("native", "qpctime", 0, path) != 0;
		if (qpctime)
			log_printf("clock: timeGetTime now comes from the performance counter ([native] qpctime=1)");
	}
	if (!qpctime)
		return real;
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return qpc_real_start + (DWORD)((q.QuadPart - qpc_start) * 1000 / qpc_freq);
}

DWORD WINAPI timeGetTime(void)
{
	timer_resolution_once();
	DWORD t = precise_time(((DWORD(WINAPI *)(void))winmm_real[WINMM_IDX_timeGetTime])());
	static volatile LONG busy;
	if (clocklog != 0 && InterlockedCompareExchange(&busy, 1, 0) == 0) {
		clock_probe(t);
		InterlockedExchange(&busy, 0);
	}
	return t;
}
