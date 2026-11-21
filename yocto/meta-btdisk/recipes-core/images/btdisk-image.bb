SUMMARY = "btdisk car stick: read-only image"
DESCRIPTION = "Boots straight into btdisk: the radio sees a USB stick within seconds of power, \
the phone connects when bluetoothd is up. Read-only root, pairings on /data, log in RAM. \
DEV=1 adds Wi-Fi (power save) and ssh."
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += "read-only-rootfs"
# Root on the serial console without a password: the console is the GPIO header inside the
# car's dashboard, and it is how a stick with no network is looked at.
IMAGE_FEATURES += "allow-empty-password empty-root-password allow-root-login"
IMAGE_FEATURES:append:btdisk-dev = " ssh-server-dropbear"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    btdisk \
    btdisk-base \
    lame \
    bluez5 \
    bluez-firmware-rpidistro-bcm43430a1-hcd \
    kernel-module-hci-uart \
    kernel-module-btbcm \
    ${BTDISK_DEV_PACKAGES} \
    ${CORE_IMAGE_EXTRA_INSTALL} \
"
BTDISK_DEV_PACKAGES = ""
BTDISK_DEV_PACKAGES:btdisk-dev = " \
    linux-firmware-rpidistro-bcm43430 \
    kernel-module-brcmfmac \
    wpa-supplicant \
    iw \
"
IMAGE_LINGUAS = ""
# udev's hardware database (20 MB, read at every boot) names PCI/USB devices; a stick has none.
BAD_RECOMMENDATIONS += "udev-hwdb"

# Units a read-only stick never needs, each one a few hundred ms of the Zero W's boot.
btdisk_trim_boot() {
    for u in systemd-journal-flush.service systemd-update-utmp.service \
             systemd-update-utmp-runlevel.service systemd-random-seed.service \
             systemd-pstore.service systemd-hwdb-update.service systemd-update-done.service \
             systemd-journal-catalog-update.service sys-kernel-debug.mount \
             sys-kernel-tracing.mount sys-fs-fuse-connections.mount dev-mqueue.mount \
             systemd-network-generator.service run-postinsts.service getty@tty1.service; do
        systemctl --root=${IMAGE_ROOTFS} mask $u >/dev/null 2>&1 || true
    done
}
ROOTFS_POSTPROCESS_COMMAND += "btdisk_trim_boot;"

WKS_FILE = "${BTDISK_WKS}"
IMAGE_FSTYPES = "wic.bz2 wic.bmap"
WIC_CREATE_EXTRA_ARGS += "--no-fstab-update"
