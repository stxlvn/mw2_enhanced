// PR checks demo (throwaway)
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "log.h"

#define LOG_MAX_BYTES (5u * 1024 * 1024)

static HANDLE log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION log_lock;

void log_open(void)
{
	char dir[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
	while (n > 0 && dir[n - 1] != '\\')
		n--;
	dir[n] = 0;
	lstrcatA(dir, "mw2coop");
	CreateDirectoryA(dir, NULL);

	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\mw2coop.log", dir);
	// The recorder writes about 1 MB an hour and nothing else empties the file: past 5 MB the old log is
	// set aside (the one before it is replaced) and a new one starts.
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (GetFileAttributesExA(path, GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > LOG_MAX_BYTES)) {
		char old[MAX_PATH];
		snprintf(old, sizeof(old), "%s.old", path);
		MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
	}
	InitializeCriticalSection(&log_lock);
	log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void log_raw(const char *text)
{
	if (log_file == INVALID_HANDLE_VALUE)
		return;
	EnterCriticalSection(&log_lock);
	DWORD written;
	WriteFile(log_file, text, lstrlenA(text), &written, NULL);
	LeaveCriticalSection(&log_lock);
}

void log_printf(const char *fmt, ...)
{
	char line[2048];
	SYSTEMTIME t;
	GetLocalTime(&t);
	int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond,
			 t.wMilliseconds, GetCurrentThreadId());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	line[n] = 0;
	log_raw(line);
}
