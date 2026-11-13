#!/bin/sh
# Audio QA for the downed player in the native split screen (docs: tools/wav_clicks.py): runs the "downaudio" scenario, records what the game plays (WASAPI loopback) and counts the clicks
# before / while / after player 2 is down.
# Usage: tools/qa_audio.sh [variant]   variant: "" (as it is), nobreath, noshock, noeq (see downaudio in _mw2coop_qa.gsc)
# Needs: <scratch>/audio/loopback_rec.exe and setvol.exe (build tools/loopback_rec.c; setvol: local/scratch/audio/setvol.c). The test copy's mixer entry is unmuted for the run and muted again afterwards.
VARIANT=${1:-}
REC=${MW2COOP_REC_S:-135} # seconds recorded (a slow level, e.g. af_chase, needs about 190)
HERE=$(cd "$(dirname "$0")" && pwd)
AUD=${MW2COOP_AUDIO_TOOLS:-$HERE/../../scratch/audio}
LOG=${MW2COOP_TEST_ROOT:-/d/coop-test}/${MW2COOP_COPY:-mw2-64}/mw2coop/mw2coop.log
W=$(cygpath -w "$HERE/keep_front.ps1")
OUT=$AUD/qa_${VARIANT:-plain}.wav
(MW2COOP_MAP=${MW2COOP_MAP:-trainer} MW2COOP_PRE="set mw2coop_qa ${MW2COOP_QA_SCEN:-downaudio};set mw2coop_qa_variant ${VARIANT:-none}" timeout $((REC + 190)) sh "$HERE/qa_native.sh" $((REC - 15)) "${MW2COOP_EXTRA:-}" > "$AUD/qa_${VARIANT:-plain}.txt" 2>&1 &)
sleep 26
PID=$(tasklist | grep -i "iw4sp.exe" | awk '{print $2}' | head -1)
"$AUD/setvol.exe" "$PID" 0 1.0
(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$W" -Seconds $((REC + 5)) > /dev/null 2>&1 &)
T0=$(date +%s.%N)
"$AUD/loopback_rec.exe" "$OUT" "$REC" 2>&1 | tail -1
"$AUD/setvol.exe" "$PID" 1 0.0
# the second of the down event in the file: the log's time of day against the recording's start (the script prints when it started)
DOWN=$(grep "downaudio: player 2 goes down" "$LOG" | tail -1 | sed 's/^\([0-9:.]*\).*/\1/')
START=$(date -d "@${T0%.*}" +%H:%M:%S)
python -I - "$OUT" "$DOWN" "$START" "$T0" <<'EOF'
import sys
f, down, start, t0 = sys.argv[1:5]
def sec(t):
    h, m, s = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)
frac = float(t0) - int(float(t0))
d = sec(down) - (sec(start) + frac)
print("variant file", f, "down event at", round(d, 1), "s")
open(f + ".down", "w").write(str(d))
EOF
python -I "$HERE/wav_window.py" "$OUT" "$(cat "$OUT.down")"
