SUMMARY = "btdisk system integration: its init, pairings on /data, a volatile journal"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://btdisk-init \
    file://btdisk-data \
    file://btdisk-data.service \
    file://journald-volatile.conf \
"
S = "${WORKDIR}"

inherit systemd allarch

do_install() {
    install -d ${D}${libexecdir}/btdisk ${D}${systemd_system_unitdir} \
               ${D}${systemd_unitdir}/journald.conf.d ${D}/data
    install -m 0755 btdisk-data ${D}${libexecdir}/btdisk/
    install -d ${D}${base_sbindir}
    install -m 0755 btdisk-init ${D}${base_sbindir}/
    install -m 0644 btdisk-data.service ${D}${systemd_system_unitdir}/
    install -m 0644 journald-volatile.conf ${D}${systemd_unitdir}/journald.conf.d/50-btdisk.conf
}

FILES:${PN} += "${systemd_unitdir}/journald.conf.d /data ${base_sbindir}/btdisk-init"
SYSTEMD_SERVICE:${PN} = "btdisk-data.service"
RDEPENDS:${PN} = "e2fsprogs-e2fsck util-linux-mount util-linux-agetty dbus-tools kmod busybox"
