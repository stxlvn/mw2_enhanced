#!/bin/sh
# Build build/winmm.dll (x64) with llvm-mingw (https://github.com/mstorsjo/llvm-mingw).
# Usage: ./build.sh [release]      ./build.sh x86 [release] builds the 32-bit game's build/x86/winmm.dll
#   LLVM_MINGW  llvm-mingw folder (default: ../tools/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64)
#   release     optimized and stripped: no debug info and no local paths inside the DLL
set -e
cd "$(dirname "$0")"
LLVM_MINGW=${LLVM_MINGW:-../tools/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64}
CC="$LLVM_MINGW/bin/x86_64-w64-mingw32-clang"
SRC="src/main.c src/d3dcap.c src/log.c src/proxy.c src/hooks.c src/hook.c src/game.c src/control.c src/discord.c src/crash.c src/scripts.c src/localize.c src/memdump.c src/menus.c src/mirror.c src/transplant.c src/access.c src/settings.c src/inimerge.c src/inisync.c src/voicefix.c src/upnp.c src/steamnet.c src/firewall.c src/zones.c src/missions.c src/menutable.c src/logo.c src/ctexts.c src/addr.c src/net.c src/netguard.c src/fonts.c src/devices.c src/console.c src/native.c src/guards.c src/climbads.c src/window.c
	src/party.c src/progress.c src/scrdebug.c src/scrdebug_stack.c src/shot.c src/fov.c src/padicon.c src/padopts.c src/gfxopts.c src/enhopts.c src/upscale.c src/fsr3.c src/padglyphs.c src/rawinput.c src/weaponfov.c src/ctxdiff.c src/inputlog.c src/loadprof.c src/rumble.c src/dx/state11.c src/dx/stateprobe.c src/dx/sm3.c src/dx/mirror11.c src/dx/d3dcallers.c src/dx/natgfx.c src/dx/natcheck.c src/winmm_stubs.S src/winmm.def"
if [ "$1" = x86 ]; then
	STRIP=; { [ "$2" = release ] || [ "$3" = release ]; } && STRIP=-s   # release: stripped, no local paths in the DLL
	# the 32-bit build of the game (see docs/32bit-plan.md): only the first slice is ported so far
	CC32="$LLVM_MINGW/bin/i686-w64-mingw32-clang"
	# the iw4x-sp client (1.0.159 iw4sp.exe) is handled by this same 32-bit DLL (it recognises the client at run time)
	OUT=build/x86; DEF=
	if [ "$2" = iw4x ]; then echo "(no separate iw4x build any more: the 32-bit DLL supports the iw4x-sp client)"; fi
	mkdir -p $OUT
	"$CC32" -O2 $STRIP -Wall -Wextra $DEF -shared -static -static-libgcc -Isrc/x86 -Isrc -Ithird_party/fidelityfx/api/include -Ithird_party/fidelityfx/upscalers/include -o $OUT/winmm.dll 		src/x86/main32.c src/x86/stubs32.c src/guards.c src/climbads.c src/log.c src/proxy.c src/hooks.c src/hook.c src/game.c src/scripts.c src/zones.c src/missions.c src/menutable.c src/logo.c src/ctexts.c src/addr.c 		src/control.c src/discord.c src/party.c src/settings.c src/inimerge.c src/inisync.c src/voicefix.c src/upnp.c src/crash.c src/fov.c src/mirror.c src/localize.c src/progress.c src/fonts.c src/devices.c src/console.c src/native.c src/addr159.c src/menus.c src/padicon.c src/padopts.c src/gfxopts.c src/enhopts.c src/upscale.c src/fsr3.c src/padglyphs.c src/rawinput.c src/weaponfov.c src/ctxdiff.c src/inputlog.c src/loadprof.c src/rumble.c src/dx/state11.c src/dx/stateprobe.c src/dx/sm3.c src/dx/mirror11.c src/dx/d3dcallers.c src/dx/natgfx.c src/dx/natcheck.c src/access.c src/net.c src/netguard.c src/steamnet.c src/firewall.c src/memdump.c src/transplant.c src/window.c src/shot.c src/d3dcap.c src/x86/scrdebug32.c src/scrdebug_stack.c src/x86/winmm_stubs.S src/x86/winmm.def -lbcrypt -lws2_32 -liphlpapi -lgdi32 -lshell32
	echo "built $OUT/winmm.dll"
	exit 0
fi
mkdir -p build
if [ "$1" = release ]; then
	"$CC" -O2 -Wall -Wextra -shared -static -static-libgcc -Isrc -Ithird_party/fidelityfx/api/include -Ithird_party/fidelityfx/upscalers/include -s -o build/winmm.dll $SRC -lws2_32 -liphlpapi -lgdi32 -lshell32
else
	"$CC" -O2 -g -Wall -Wextra -shared -static -static-libgcc -Isrc -Ithird_party/fidelityfx/api/include -Ithird_party/fidelityfx/upscalers/include -o build/winmm.dll $SRC -lws2_32 -liphlpapi -lgdi32 -lshell32 \
		-Wl,--pdb=build/winmm.pdb
fi
echo "built build/winmm.dll${1:+ ($1)}"
