FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://main.conf file://bluetooth-btdisk.conf"

# meta-raspberrypi attaches the radio with pi-bluetooth's hciuart.service; the stick lets the
# kernel do it (dtparam=krnbt=on: hci_uart over serdev), which is faster and never waits 90 s.
RDEPENDS:${PN}:remove:rpi = "pi-bluetooth"

do_install:append() {
    install -m 0644 ${WORKDIR}/main.conf ${D}${sysconfdir}/bluetooth/main.conf
    install -d ${D}${systemd_system_unitdir}/bluetooth.service.d
    install -m 0644 ${WORKDIR}/bluetooth-btdisk.conf ${D}${systemd_system_unitdir}/bluetooth.service.d/50-btdisk.conf
}
FILES:${PN} += "${systemd_system_unitdir}/bluetooth.service.d"
