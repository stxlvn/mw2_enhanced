#pragma once
// A disk cache for compiled shaders (included by nat11.c and nat12.c): the translated HLSL of a shader is compiled the first time the game uses it, which stalls the frame for tens of
// milliseconds each (a new effect that appears in a level: snow, a sandstorm, water). The compiled code is kept in mw2coop\shadercache\<hash>.cso, keyed by the HLSL text and the stage:
// the next run (and every later one) loads it instead. A file that does not match (length or second hash) or is cut short is ignored and compiled again.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static uint64_t nc_fnv(const char *s, size_t n, uint64_t h)
{
	for (size_t i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * 0x100000001b3ull;
	return h;
}

static void nc_path(char *out, size_t size, const char *hlsl, int is_vs, uint64_t *check)
{
	const size_t n = strlen(hlsl);
	const uint64_t h = nc_fnv(hlsl, n, 0xcbf29ce484222325ull ^ (is_vs ? 0x5656 : 0x5050));
	*check = nc_fnv(hlsl, n, 0x84222325cbf29ce4ull) ^ n;
	snprintf(out, size, "mw2coop\\shadercache\\%016llx.cso", (unsigned long long)h);
}

// the compiled code of the HLSL, or NULL: the caller frees it
static void *nc_load(const char *hlsl, int is_vs, size_t *size)
{
	char path[96];
	uint64_t check;
	nc_path(path, sizeof(path), hlsl, is_vs, &check);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint64_t head[2] = {0, 0};
	void *data = NULL;
	if (fread(head, sizeof(head), 1, f) == 1 && head[0] == check && head[1] > 16 && head[1] < (64u << 20)) {
		data = malloc((size_t)head[1]);
		if (data && fread(data, 1, (size_t)head[1], f) != (size_t)head[1]) {
			free(data);
			data = NULL;
		}
	}
	fclose(f);
	if (data)
		*size = (size_t)head[1];
	return data;
}

static void nc_store(const char *hlsl, int is_vs, const void *data, size_t size)
{
	char path[96], tmp[112];
	uint64_t check;
	nc_path(path, sizeof(path), hlsl, is_vs, &check);
	CreateDirectoryA("mw2coop", NULL);
	CreateDirectoryA("mw2coop\\shadercache", NULL);
	snprintf(tmp, sizeof(tmp), "%s.%lx", path, (unsigned long)GetCurrentThreadId());
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	const uint64_t head[2] = {check, (uint64_t)size};
	const int ok = fwrite(head, sizeof(head), 1, f) == 1 && fwrite(data, 1, size, f) == size;
	fclose(f);
	if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
		DeleteFileA(tmp);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ background warm-up
// The game's shaders are known when its assets load, long before the first draw that uses them. Each one is queued here and two low-priority worker threads translate and compile the variant
// the game uses first (a vertex shader without integer inputs, a pixel shader without hardware shadow maps) into the disk cache, so the render thread finds the code ready (a first-ever run still
// compiles what the workers have not reached yet, and the variants that are not the common one).
typedef struct nc_job {
	struct nc_job *next;
	uint32_t *code;
	size_t dwords;
} nc_job;
static nc_job *nc_head, *nc_tail;
static CRITICAL_SECTION nc_lock;
static HANDLE nc_event;
static LONG nc_started;
static void (*nc_work)(const uint32_t *code, size_t dwords);

static DWORD WINAPI nc_thread(void *unused)
{
	(void)unused;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	for (;;) {
		EnterCriticalSection(&nc_lock);
		nc_job *j = nc_head;
		if (j) {
			nc_head = j->next;
			if (!nc_head)
				nc_tail = NULL;
		}
		LeaveCriticalSection(&nc_lock);
		if (!j) {
			WaitForSingleObject(nc_event, INFINITE);
			continue;
		}
		nc_work(j->code, j->dwords);
		free(j->code);
		free(j);
	}
	return 0;
}

static void nc_enqueue(const uint32_t *code, size_t dwords, void (*work)(const uint32_t *, size_t))
{
	if (!code || !dwords || getenv("MW2COOP_NOPREWARM"))
		return;
	if (InterlockedCompareExchange(&nc_started, 1, 0) == 0) {
		InitializeCriticalSection(&nc_lock);
		nc_event = CreateEventA(NULL, FALSE, FALSE, NULL); // (auto reset: one worker wakes per signal)
		nc_work = work;
		for (int i = 0; i < 2; i++)
			CloseHandle(CreateThread(NULL, 0, nc_thread, NULL, 0, NULL));
	}
	while (!nc_work || !nc_event) // (the first caller is still setting up)
		Sleep(0);
	nc_job *j = calloc(1, sizeof(*j));
	j->code = malloc(dwords * 4);
	memcpy(j->code, code, dwords * 4);
	j->dwords = dwords;
	EnterCriticalSection(&nc_lock);
	if (nc_tail)
		nc_tail->next = j;
	else
		nc_head = j;
	nc_tail = j;
	LeaveCriticalSection(&nc_lock);
	SetEvent(nc_event);
}
