// Crash locations for QA: a vectored exception handler logs access violations and other fatal
// exceptions with the faulting instruction as an RVA of iw4sp.exe and the return addresses
// into the exe found on the stack, then lets the exception go on as before (the game's own
// handlers or Windows still decide what happens).
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <tlhelp32.h>

#include "crash.h"
#include "game.h"
#include "hook.h"
#include "native.h"
#include "scrdebug_stack.h"
#include "log.h"

// the registers of the exception context: 64 bits Rip/Rsp..., 32 bits Eip/Esp...
#ifdef _WIN64
#define CTX_IP(c) ((c)->Rip)
#define CTX_SP(c) ((c)->Rsp)
#else
#define CTX_IP(c) ((c)->Eip)
#define CTX_SP(c) ((c)->Esp)
#endif

static uintptr_t image_base, image_end;
static volatile LONG logged;

static int in_image(uintptr_t p)
{
	return p >= image_base && p < image_end;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info)
{
	DWORD code = info->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
	    code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
		return EXCEPTION_CONTINUE_SEARCH;
	CONTEXT *c = info->ContextRecord;
	uintptr_t rip = (uintptr_t)CTX_IP(c);
	{ // a fault inside kernel32 / kernelbase is the probe of IsBadReadPtr, IsBadStringPtr or lstrcpy, which catch it themselves: it is not a crash, and it must not use up the three entries below
		HMODULE mod = NULL;
		char name[MAX_PATH];
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod) && mod && GetModuleFileNameA(mod, name, sizeof(name))) {
			const char *base_name = strrchr(name, 0x5c);
			base_name = base_name ? base_name + 1 : name;
			if (!lstrcmpiA(base_name, "kernel32.dll") || !lstrcmpiA(base_name, "kernelbase.dll"))
				return EXCEPTION_CONTINUE_SEARCH;
		}
	}
	if (InterlockedIncrement(&logged) > 3)
		return EXCEPTION_CONTINUE_SEARCH;
	ULONG_PTR *ei = info->ExceptionRecord->ExceptionInformation;
	if (code == EXCEPTION_ACCESS_VIOLATION)
		log_printf("CRASH: access violation (%s 0x%llx) at %s 0x%llx", ei[0] == 1 ? "write" : ei[0] == 8 ? "execute" : "read",
			   (unsigned long long)ei[1], in_image(rip) ? "rva" : "address",
			   (unsigned long long)(in_image(rip) ? rip - image_base : rip));
	else
		log_printf("CRASH: exception 0x%lx at %s 0x%llx", code, in_image(rip) ? "rva" : "address",
			   (unsigned long long)(in_image(rip) ? rip - image_base : rip));
#ifdef _WIN64
	log_printf("CRASH: rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx",
		   c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9);
#else
	log_printf("CRASH: eax %lx ebx %lx ecx %lx edx %lx esi %lx edi %lx ebp %lx", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi,
		   c->Ebp);
#endif
	// return addresses into the exe among the first stack slots (a rough call stack)
	const uintptr_t *sp = (const uintptr_t *)CTX_SP(c);
	char line[512];
	int n = 0, found = 0;
	n += wsprintfA(line, "CRASH: stack rvas:");
	for (int i = 0; i < 512 && found < 16; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		if (i % 64 == 0 && (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT))
			break;
		uintptr_t v = sp[i];
		if (in_image(v) && n < (int)sizeof(line) - 20) {
			n += wsprintfA(line + n, " %lx", (unsigned long)(v - image_base));
			found++;
		}
	}
	log_printf("%s", line);
	{ // the same, for code outside the exe (a graphics driver, the Steam overlay, d3d9): module + offset of the faulting instruction and of the return addresses on the stack
		char mods[900];
		int m = 0;
		uintptr_t list[17];
		int nl = 0;
		list[nl++] = rip;
		for (int i = 0; i < 4096 && nl < 17; i++) {
			MEMORY_BASIC_INFORMATION mbi;
			if (i % 64 == 0 && (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT))
				break;
			uintptr_t v = sp[i];
			if (v > 0x10000 && VirtualQuery((void *)v, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
				list[nl++] = v;
		}
		m += wsprintfA(mods, "CRASH: modules:");
		for (int i = 0; i < nl && m < (int)sizeof(mods) - 80; i++) {
			HMODULE mod = NULL;
			char name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)list[i], &mod) && mod) {
				GetModuleFileNameA(mod, name, sizeof(name));
				const char *slash = strrchr(name, '\\');
				m += wsprintfA(mods + m, " %s+%lx", slash ? slash + 1 : name, (unsigned long)(list[i] - (uintptr_t)mod));
			}
		}
		log_printf("%s", mods);
	}
	log_printf("CRASH: GSC position of the script VM:");
	scrdebug_stack_log();
#ifdef _WIN64
	log_printf("CRASH: native split screen: %s", native_crash_state());
#endif
	if (code == EXCEPTION_STACK_OVERFLOW) { // the frames at the stack pointer are the loop; the ones at the base of the stack (the oldest) show what started it. Last: the handler has little stack left after an overflow, and the lines above must not be lost to it
		const uintptr_t *top = (const uintptr_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
		char oldest[900];
		int m = wsprintfA(oldest, "CRASH: oldest frames (from the stack base):");
		for (int i = 1, got = 0; i < 16384 && got < 28 && m < (int)sizeof(oldest) - 80; i++) {
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t v = top[-i];
			if (v > 0x10000 && VirtualQuery((void *)v, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
				HMODULE mod = NULL;
				char name[MAX_PATH] = "?";
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &mod) && mod) {
					GetModuleFileNameA(mod, name, sizeof(name));
					const char *slash = strrchr(name, '\\');
					m += wsprintfA(oldest + m, " %s+%lx", slash ? slash + 1 : name, (unsigned long)(v - (uintptr_t)mod));
					got++;
				}
			}
		}
		log_printf("%s", oldest);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// Return addresses into the exe among the first stack slots from sp (a rough call stack).
static void stack_rvas(HANDLE out, const char *what, uintptr_t rip, const uintptr_t *sp, int slots)
{
	char line[1536];
	int n = wsprintfA(line, "%s at %s %lx, stack rvas:", what, in_image(rip) ? "rva" : "address",
			  (unsigned long)(in_image(rip) ? rip - image_base : rip));
	int found = 0;
	for (int i = 0; i < slots && found < 48; i++) {
		uintptr_t v = sp[i];
		if (in_image(v) && n < (int)sizeof(line) - 20) {
			n += wsprintfA(line + n, " %lx", (unsigned long)(v - image_base));
			found++;
		} else if (v > 0x10000 && n < (int)sizeof(line) - 60) { // code of another module (the Direct3D runtime, the driver): name+offset, to see what a stuck thread waits in
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery((void *)v, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
				char path[MAX_PATH];
				DWORD len = GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path));
				const char *name = path;
				for (DWORD k = 0; k < len; k++)
					if (path[k] == '\\')
						name = path + k + 1;
				n += wsprintfA(line + n, " %s+%lx", len ? name : "?", (unsigned long)(v - (uintptr_t)mbi.AllocationBase));
				found++;
			}
		}
	}
	lstrcatA(line, "\r\n");
	DWORD wrote;
	WriteFile(out, line, lstrlenA(line), &wrote, NULL);
}

// QA, for a hang: every other thread of the game, where it is and what called it, into
// <game dir>\mw2coop\stacks.txt. A thread of its own runs it when <game dir>\mw2coop\dumpstacks
// appears (and deletes that file); it takes no lock of the game's or the log's, which a hung game
// may hold (the control thread stopped with it, 2026-10-24).
static void dump_threads(HANDLE out)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return;
	THREADENTRY32 te = {.dwSize = sizeof(te)};
	DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
			continue;
		HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
		if (!t)
			continue;
		// copy what is needed while it is suspended, write only after it runs again (it may hold a
		// lock the writing needs)
		static uintptr_t stack[1024];
		uintptr_t rip = 0;
		int got = 0;
		if (SuspendThread(t) != (DWORD)-1) {
			CONTEXT c = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
			if (GetThreadContext(t, &c)) {
				rip = (uintptr_t)CTX_IP(&c);
				SIZE_T read = 0;
				ReadProcessMemory(GetCurrentProcess(), (void *)CTX_SP(&c), stack, sizeof(stack), &read);
				if (!read) // the stack's end is near: read what is there
					for (size_t want = sizeof(stack) / 2; want >= 64 && !read; want /= 2)
						ReadProcessMemory(GetCurrentProcess(), (void *)CTX_SP(&c), stack, want, &read);
				got = (int)(read / sizeof(uintptr_t));
			}
			ResumeThread(t);
		}
		if (rip) {
			char what[64];
			wsprintfA(what, "stacks: thread %lu", te.th32ThreadID);
			stack_rvas(out, what, rip, stack, got);
		}
		CloseHandle(t);
	}
	CloseHandle(snap);
}

#ifndef _WIN64
// 32-bit, for a hang in DB_FindXAssetHeader (0x4f6f10: the caller waits until the database thread has the asset): the request
// slot it fills first, PTR_DAT_009d7c80 = the asset's table entry (+4 the header, whose first field is usually the name),
// DAT_00c3e85c = 1, DAT_00a961a4 = the answer of 0x4d3e70.
static void dump_db_state(HANDLE out)
{
	if (game_is_iw4x()) {
		(void)out; // these addresses are the official build's; not mapped for the 1.0.159 exe
		return;
	}
	const uint8_t *exe = (const uint8_t *)image_base;
	char line[512];
	int n = 0;
	const uint8_t *entry = *(const uint8_t *const *)(exe + (0x9d7c80 - 0x400000));
	MEMORY_BASIC_INFORMATION mbi;
	n += wsprintfA(line + n, "db: request entry %p, flag %lu, status %lu", (const void *)entry, (unsigned long)*(const uint32_t *)(exe + (0xc3e85c - 0x400000)),
		       (unsigned long)*(const uint32_t *)(exe + (0xa961a4 - 0x400000)));
	if (entry && VirtualQuery(entry, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
		const uint8_t *header = *(const uint8_t *const *)(entry + 4);
		n += wsprintfA(line + n, ", entry bytes %02x%02x%02x%02x %02x%02x%02x%02x flags %02x %02x %02x, header %p", entry[0], entry[1], entry[2], entry[3], entry[4],
			       entry[5], entry[6], entry[7], entry[8], entry[9], entry[10], (const void *)header);
		if (header && VirtualQuery(header, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
			const char *name = *(const char *const *)header;
			if (name && VirtualQuery(name, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
				n += wsprintfA(line + n, ", name '%.60s'", name);
		}
	}
	lstrcatA(line, "\r\n");
	DWORD wrote;
	WriteFile(out, line, lstrlenA(line), &wrote, NULL);
}
#endif

#ifdef _WIN64
// The game's CRT abort() (rva 0x303a64) ends in __fastfail (int 0x29, code 0xc0000409), which no exception handler sees: the native split screen runs of
// 2026-10-28 ended that way with nothing in the log. The replacement logs who called it and the stack, then ends the game the way abort() does (exit code 3).
static void abort_hook(void)
{
	uintptr_t from = (uintptr_t)__builtin_return_address(0);
	log_printf("CRASH: abort() called from %s %llx", in_image(from) ? "rva" : "address", (unsigned long long)(in_image(from) ? from - image_base : from));
	const uintptr_t *sp = (const uintptr_t *)__builtin_frame_address(0);
	char line[512];
	int n = wsprintfA(line, "CRASH: stack rvas:"), found = 0;
	for (int i = 0; i < 1024 && found < 24; i++) {
		MEMORY_BASIC_INFORMATION mbi;
		if (i % 64 == 0 && (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT))
			break;
		if (in_image(sp[i]) && n < (int)sizeof(line) - 20) {
			n += wsprintfA(line + n, " %lx", (unsigned long)(sp[i] - image_base));
			found++;
		}
	}
	log_printf("%s", line);
	log_printf("CRASH: native split screen: %s", native_crash_state());
	TerminateProcess(GetCurrentProcess(), 3);
}
#endif

static char flag_path[MAX_PATH], out_path[MAX_PATH];

static DWORD WINAPI watchdog(LPVOID arg)
{
	(void)arg;
	for (;;) {
		Sleep(500);
		if (!DeleteFileA(flag_path))
			continue;
		HANDLE out = CreateFileA(out_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
		if (out == INVALID_HANDLE_VALUE)
			continue;
		dump_threads(out);
#ifndef _WIN64
		dump_db_state(out);
#endif
		CloseHandle(out);
	}
	return 0;
}

#ifdef _WIN64
// The engine's error function (rva 0x1fb5f0, Com_Error(level, format, ...)): the message and the call stack go into the log before the engine handles it (a drop error is turned into
// "Failed init after: <message>" and the game ends when it happens while the game is still starting, which left a vid_restart on a 9Ex device with no trace of what was wrong).
// The arguments are passed on as they are (the callee stores the register ones in its home space; the stack ones are read where the caller left them).
static void (*com_error_original)(int, const char *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static void com_error_logged(int level, const char *fmt, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
{
	static volatile LONG logged;
	if (InterlockedIncrement(&logged) <= 20) {
		char text[300];
		const uintptr_t args[3] = {a, b, c};
		int n = wsprintfA(text, "engine error (level %d): %.120s", level, fmt && !IsBadStringPtrA(fmt, 8) ? fmt : "?");
		for (int i = 0; i < 3 && n < (int)sizeof(text) - 90; i++) { // an argument that reads as text is shown as text, the others as numbers
			const char *as_text = (const char *)args[i];
			if (args[i] > 0x10000 && !IsBadStringPtrA(as_text, 4) && as_text[0] >= 32 && as_text[0] < 127)
				n += wsprintfA(text + n, " | '%.50s'", as_text);
			else
				n += wsprintfA(text + n, " | %lx", (unsigned long)args[i]);
		}
		void *frames[20];
		USHORT got = CaptureStackBackTrace(1, 20, frames, NULL);
		n += wsprintfA(text + n, " | from");
		for (USHORT i = 0; i < got && n < (int)sizeof(text) - 24; i++)
			if (in_image((uintptr_t)frames[i]))
				n += wsprintfA(text + n, " %lx", (unsigned long)((uintptr_t)frames[i] - image_base));
		log_printf("%s", text);
	}
	com_error_original(level, fmt, a, b, c, d, e, f);
}
#endif

void crash_install(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
#ifdef _WIN64
	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
#else
	IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
#endif
	image_base = (uintptr_t)exe;
	image_end = image_base + nt->OptionalHeader.SizeOfImage;
	AddVectoredExceptionHandler(1, on_exception);
#ifdef _WIN64
	{ // sub rsp,28h; call; test rax,rax; jz short: 14 bytes, the jump's size (the retail exe only)
		uint8_t *ab = (uint8_t *)image_base + 0x303a64;
		static const uint8_t head[] = {0x48, 0x83, 0xec, 0x28, 0xe8};
		if (image_end - image_base > 0x303a80 && !memcmp(ab, head, sizeof(head)) && ab[9] == 0x48 && ab[10] == 0x85 && ab[11] == 0xc0 && ab[12] == 0x74)
			hook_jump(ab, 14, abort_hook, 0xcc);
	}
#endif
#ifdef _WIN64
	{ // mov [rsp+10h],rdx; mov [rsp+18h],r8; mov [rsp+20h],r9: 15 bytes, three whole instructions (the retail exe only)
		static const uint8_t head[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x4c, 0x89, 0x4c, 0x24, 0x20};
		if (image_end - image_base > 0x1fb700 && hook_function((uint8_t *)image_base + 0x1fb5f0, head, sizeof(head), (void *)com_error_logged, (void **)&com_error_original))
			log_printf("crash: the engine's error function logs its messages");
	}
#endif
	DWORD n = GetModuleFileNameA(NULL, flag_path, MAX_PATH);
	while (n > 0 && flag_path[n - 1] != '\\')
		n--;
	flag_path[n] = 0;
	lstrcpynA(out_path, flag_path, MAX_PATH);
	lstrcatA(flag_path, "mw2coop\\dumpstacks");
	lstrcatA(out_path, "mw2coop\\stacks.txt");
	CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
	log_printf("crash: exception logger installed");
}
