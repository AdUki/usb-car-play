FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://btdisk.cfg"
# CONFIG_KERNEL_LZ4 (btdisk.cfg) compresses the zImage with the host's lz4.
DEPENDS += "lz4-native"
