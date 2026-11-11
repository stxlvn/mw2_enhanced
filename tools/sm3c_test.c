// Runs the C translator (src/dx/sm3.c) over a folder of dumped shaders (MW2COOP_DUMPSHADERS=1) and writes <name>.hlsl for each into an output folder; tools/sm3c_compare.py compares those with the
// Python translator (tools/sm3_to_hlsl.py) and compiles both. Windows only.
//   sm3c_test <shader folder> <output folder>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dx/sm3.h"

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: sm3c_test <shader folder> <output folder>\n");
		return 2;
	}
	char pat[MAX_PATH];
	snprintf(pat, sizeof(pat), "%s\\*.bin", argv[1]);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pat, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return 1;
	int ok = 0, bad = 0;
	do {
		if (strncmp(fd.cFileName, "vs_", 3) && strncmp(fd.cFileName, "ps_", 3))
			continue;
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\%s", argv[1], fd.cFileName);
		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		uint32_t *buf = malloc(n + 4);
		fread(buf, 1, n, f);
		fclose(f);
		char *hlsl = NULL, err[128] = "";
		int vs = 0;
		if (sm3_translate(buf, n / 4, &hlsl, &vs, err, sizeof(err))) {
			snprintf(path, sizeof(path), "%s\\%s.hlsl", argv[2], fd.cFileName);
			FILE *o = fopen(path, "wb");
			fwrite(hlsl, 1, strlen(hlsl), o);
			fclose(o);
			free(hlsl);
			ok++;
		} else {
			printf("%s: %s\n", fd.cFileName, err);
			bad++;
		}
		free(buf);
	} while (FindNextFileA(h, &fd));
	printf("%d translated, %d failed\n", ok, bad);
	return 0;
}
