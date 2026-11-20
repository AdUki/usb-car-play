#!/bin/bash
# bench-test.sh BENCH ARM_BUILD HOST_BUILD — btdisk end to end on the bench's Pi Zero W.
#
#   phone: PHONE=bench (default): another bench board (SOURCE=btbench-rpi5, paired to the bench Pi)
#          connects and plays a 1 kHz tone over A2DP, its player answers AVRCP;
#          PHONE=real: your phone, paired to the bench Pi; PHONE=fake: fake-phone on this PC;
#          PHONE=tone: no Bluetooth, btdisk plays a 1 kHz sine itself (the USB path alone)
#     -> A2DP + AVRCP -> btdisk on the Pi -> FunctionFS -> dummy_hcd -> usb-storage -> vfat
#     -> radio-play (reads the WAVs like a head unit: Next, restart = Previous, Previous)
#
# What it changes on the bench, and puts back on exit (also on failure or Ctrl-C): PipeWire and
# WirePlumber are stopped (btdisk is the A2DP sink), usb_f_fs and dummy_hcd are loaded and a
# gadget "btdisk" is made. The bench's own gadget, agent, name and pairings are left alone.
set -euo pipefail

BENCH=${1:?usage: bench-test.sh BENCH ARM_BUILD HOST_BUILD}
ARM=${2:?}
HOST=${3:?}
PHONE=${PHONE:-bench}
SOURCE=${SOURCE:-btbench-rpi5}
BENCH_REPO=${BENCH_REPO:-$HOME/projects/bt-developement-bench}
bench() { (cd "$BENCH_REPO" && timeout 30 tools/bench -b "$@"); }
SECONDS_PER_STEP=${SECONDS_PER_STEP:-20}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$HERE/.bench/run-$(date +%Y%m%d-%H%M%S)}
SSH_OPTS="-o ControlMaster=auto -o ControlPath=/tmp/ssh-btdisk-%C -o ControlPersist=60 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
on() { ssh $SSH_OPTS root@$BENCH "$@"; }

mkdir -p "$OUT"
"$HERE/tools/bench-modules.sh" "$BENCH"
mods=$HERE/.bench/$(on uname -r)
addr=$(on "busctl get-property org.bluez /org/bluez/hci0 org.bluez.Adapter1 Address" | tr -d '"' | cut -d" " -f2)
echo "bench $BENCH ($addr), results in $OUT"

cleanup() {
    set +e
    [ -n "${phone_pid:-}" ] && kill "$phone_pid" 2>/dev/null
    if [ -n "${tone_stream:-}" ]; then
        bench "$SOURCE" stop "$tone_stream" >/dev/null 2>&1
        bench "$SOURCE" disconnect "$addr" >/dev/null 2>&1
    fi
    on 'cp /tmp/btdisk/log /tmp/btdisk/log.last 2>/dev/null; umount /tmp/radio 2>/dev/null
        pkill -TERM -x btdisk; sleep 1; /tmp/btdisk/btdisk-gadget teardown
        rmmod dummy_hcd usb_f_fs 2>/dev/null
        systemctl start btbench-audio-pipewire.target' >/dev/null 2>&1
    scp $SSH_OPTS -q root@$BENCH:/tmp/btdisk/log.last "$OUT/btdisk.log" 2>/dev/null
    echo "bench restored (PipeWire back, modules and gadget removed)"
}
trap cleanup EXIT

cat > "$OUT/bench.conf" <<'EOF'
# btdisk on the bench: the "radio" is the Pi's own kernel through dummy_hcd.
[usb]
udc = dummy_udc.0
[player]
wav_info = true
[bluetooth]
# The bench keeps its own agent, name, discoverability and reconnects.
agent = false
alias = btbench-rpi
discoverable_s = -1
reconnect_s = 0
EOF
on 'mkdir -p /tmp/btdisk /tmp/radio'
scp $SSH_OPTS -q "$ARM/btdisk.stripped" root@$BENCH:/tmp/btdisk/btdisk
scp $SSH_OPTS -q "$ARM/radio-play.stripped" root@$BENCH:/tmp/btdisk/radio-play
scp $SSH_OPTS -q "$HERE/scripts/btdisk-gadget" "$OUT/bench.conf" "$mods/usb_f_fs.ko" "$mods/dummy_hcd.ko" \
    root@$BENCH:/tmp/btdisk/

on 'cd /tmp/btdisk && insmod usb_f_fs.ko && insmod dummy_hcd.ko &&
    systemctl stop btbench-audio-pipewire.target pipewire.service wireplumber.service pipewire.socket pipewire-manager.socket &&
    ./btdisk-gadget setup && (nohup ./btdisk -c bench.conf -v '"$([ "$PHONE" = tone ] && echo --tone 1000)"' > log 2>&1 &) &&
    for i in $(seq 40); do [ -b /dev/sda1 ] && break; sleep 0.25; done &&
    mount -o ro /dev/sda1 /tmp/radio && ls /tmp/radio'

if [ "$PHONE" = fake ]; then
    # Note: a PC whose login screen runs its own PipeWire (Ubuntu's gdm) owns the A2DP source
    # endpoint there, and BlueZ refuses to hand the stream to fake-phone (EPERM).
    "$HOST/fake-phone" "$addr" --seconds $((SECONDS_PER_STEP * 4 + 40)) > "$OUT/phone.log" 2>&1 &
    phone_pid=$!
elif [ "$PHONE" = bench ]; then
    node=bluez_output.${addr//:/_}.1
    bench "$SOURCE" connect "$addr" | tail -1
    for i in $(seq 30); do bench "$SOURCE" sinks | grep -q "$node" && break; sleep 0.5; done
    tone_stream=$(bench "$SOURCE" play tone 1000 "$node" | sed -n 's/^stream \([0-9]*\).*/\1/p')
    echo "$SOURCE plays a 1 kHz tone to the stick (stream $tone_stream)"
elif [ "$PHONE" = real ]; then
    echo "play music on your phone, connected to $BENCH; waiting for its stream"
fi
if [ "$PHONE" != tone ]; then
    for i in $(seq ${STREAM_WAIT:-60}); do on 'grep -q "a2dp: streaming" /tmp/btdisk/log' && break; sleep 1; done
    on 'grep -q "a2dp: streaming" /tmp/btdisk/log' || { echo "no A2DP stream arrived"; exit 1; }
fi
sleep 2

# The radio: file 1, Next to file 2, Previous by restarting file 2, Previous to file 1.
on "cd /tmp/radio && /tmp/btdisk/radio-play --seconds $SECONDS_PER_STEP --out /tmp/btdisk/radio.raw \
    'Bluetooth 01.wav' 'Bluetooth 02.wav' 'Bluetooth 02.wav' 'Bluetooth 01.wav'" | tee "$OUT/radio.txt"
scp $SSH_OPTS -q root@$BENCH:/tmp/btdisk/radio.raw "$OUT/radio.raw"
on 'grep -vE "jump|dropped" /tmp/btdisk/log' > "$OUT/btdisk.log" || true
echo "--- what the radio played"
"$HOST/radio-play" --analyze "$OUT/radio.raw" | tee "$OUT/analysis.txt"
echo "--- btdisk's decisions"
grep -E "radio |-> phone|metadata|now playing|a2dp" "$OUT/btdisk.log" || true
if [ "$PHONE" = fake ]; then echo "--- the phone"; cat "$OUT/phone.log"; fi
