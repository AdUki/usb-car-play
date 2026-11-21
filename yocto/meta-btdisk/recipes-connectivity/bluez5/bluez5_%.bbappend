FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://main.conf file://bluetooth-btdisk.conf"

# A2DP delay reporting from the sink: btdisk tells the phone its delay (for A/V sync). 5.72
# can't do that from D-Bus; these backport what upstream has since 5.79, plus the initial report.
SRC_URI += " \
    file://0001-transport-Allow-a-local-A2DP-sink-to-set-its-Delay.patch \
    file://0002-avdtp-Fix-error-parsing-from-failed-delay-report-req.patch \
"

# meta-raspberrypi attaches the radio with pi-bluetooth's hciuart.service; the stick lets the
# kernel do it (dtparam=krnbt=on: hci_uart over serdev), which is faster and never waits 90 s.
RDEPENDS:${PN}:remove:rpi = "pi-bluetooth"

do_install:append() {
    install -m 0644 ${WORKDIR}/main.conf ${D}${sysconfdir}/bluetooth/main.conf
    # The mode bluetooth.service's ConfigurationDirectoryMode asks for: on the read-only root
    # systemd can't change it, and warns at every boot.
    chmod 0555 ${D}${sysconfdir}/bluetooth
    install -d ${D}${systemd_system_unitdir}/bluetooth.service.d
    install -m 0644 ${WORKDIR}/bluetooth-btdisk.conf ${D}${systemd_system_unitdir}/bluetooth.service.d/50-btdisk.conf
}
FILES:${PN} += "${systemd_system_unitdir}/bluetooth.service.d"
