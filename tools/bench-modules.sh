#!/bin/bash
# bench-modules.sh BENCH — build usb_f_fs.ko and dummy_hcd.ko for the kernel the bench's Pi Zero W
# runs, from the bench repository's kernel tree and config, without touching that repository.
#
# The bench kernel has neither (its gadget is built in and has no FunctionFS). USB_FUNCTIONFS=m,
# the legacy g_ffs option, is what makes usb_f_fs a module: configfs can use it like the built-in
# one, so the modules load into the running kernel and nothing is rebooted. The release string
# is pinned to the running kernel's, so vermagic matches.
set -euo pipefail

BENCH=${1:?usage: bench-modules.sh BENCH}
BENCH_REPO=${BENCH_REPO:-$HOME/projects/bt-developement-bench}
OUT=${OUT:-$PWD/.bench}
SSH_OPTS="-o ControlMaster=auto -o ControlPath=/tmp/ssh-btdisk-%C -o ControlPersist=60 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

release=$(ssh $SSH_OPTS root@$BENCH uname -r)
rev=${release##*-g}
dir=$OUT/$release
if [ -f "$dir/usb_f_fs.ko" ] && [ -f "$dir/dummy_hcd.ko" ]; then
    echo "modules for $release already in $dir"
    exit 0
fi
echo "building modules for $release (kernel revision $rev)"
mkdir -p "$dir/src" "$dir/obj"
git -C "$BENCH_REPO/src/linux" archive "$rev" | tar -x -C "$dir/src"
ssh $SSH_OPTS root@$BENCH 'zcat /proc/config.gz' > "$dir/obj/.config"
"$dir/src/scripts/config" --file "$dir/obj/.config" -d LOCALVERSION_AUTO --set-str LOCALVERSION "" \
    -m USB_FUNCTIONFS -e USB_FUNCTIONFS_GENERIC -m USB_DUMMY_HCD

sdk_env=$(ls "$BENCH_REPO"/sdk/rpi0w/environment-setup-* | head -1)
cross=$(. "$sdk_env" >/dev/null 2>&1 && command -v "${TARGET_PREFIX}gcc")
K=(env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CC -u LD -u ARCH -u CROSS_COMPILE
   make -C "$dir/src" O="$dir/obj" ARCH=arm CROSS_COMPILE="${cross%gcc}" LOCALVERSION="-${release#*-}")
# LOCALVERSION above is the part of the release after the version: "-btbench-g<rev>".
"${K[@]}" olddefconfig >/dev/null
[ "$("${K[@]}" -s kernelrelease)" = "$release" ] || { echo "release mismatch"; exit 1; }
"${K[@]}" -j"$(nproc)" zImage modules >/dev/null
cp "$dir/obj/drivers/usb/gadget/function/usb_f_fs.ko" "$dir/obj/drivers/usb/gadget/udc/dummy_hcd.ko" "$dir/"
rm -rf "$dir/src" "$dir/obj"
echo "modules in $dir"
