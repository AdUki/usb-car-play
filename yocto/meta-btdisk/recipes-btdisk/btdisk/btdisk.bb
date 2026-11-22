SUMMARY = "USB flash drive that plays the phone's Bluetooth audio"
DESCRIPTION = "Presents itself to a car radio as a USB stick of WAV (or MP3) files whose \
contents are the live A2DP stream from the phone; the radio's next/previous/pause become \
AVRCP commands and the track title lands in the file headers the radio reads."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# sd-bus (libsystemd, already on the image as the init's library) for BlueZ; libsbc, fdk-aac and
# libfreeaptx decode A2DP's SBC, AAC and aptX. Nothing else: no audio server, no Python.
DEPENDS = "systemd sbc fdk-aac libfreeaptx"
RDEPENDS:${PN} = "libsystemd sbc fdk-aac libfreeaptx bluez5"

# Built from app/ in this repository: the layer is yocto/meta-btdisk, four levels up from here.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${THISDIR}/../../../../:"
SRC_URI = " \
    file://app \
    file://scripts/btdisk-gadget \
    file://config/btdisk.conf \
    file://btdisk.service \
"
S = "${WORKDIR}/app"

inherit cmake systemd pkgconfig

EXTRA_OECMAKE = "-DCMAKE_BUILD_TYPE=Release -DBTDISK_TESTS=OFF -DBTDISK_TOOLS=OFF -DBTDISK_ALL_CODECS=ON"

# file://app copies the working tree as it is, local build directories included.
do_configure:prepend() {
    rm -rf ${S}/build ${S}/build-*
}

do_install:append() {
    install -d ${D}${libexecdir}/btdisk ${D}${sysconfdir} ${D}${systemd_system_unitdir}
    install -m 0755 ${WORKDIR}/scripts/btdisk-gadget ${D}${libexecdir}/btdisk/
    install -m 0644 ${WORKDIR}/config/btdisk.conf ${D}${sysconfdir}/
    install -m 0644 ${WORKDIR}/btdisk.service ${D}${systemd_system_unitdir}/
}

SYSTEMD_SERVICE:${PN} = "btdisk.service"
SYSTEMD_AUTO_ENABLE = "enable"
CONFFILES:${PN} = "${sysconfdir}/btdisk.conf"
