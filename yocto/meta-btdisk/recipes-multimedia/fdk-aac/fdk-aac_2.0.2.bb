SUMMARY = "FDK-AAC audio codec"
DESCRIPTION = "The Fraunhofer FDK AAC Codec Library for Android (\"FDK AAC Codec\"): the AAC \
encoder and decoder Android uses for A2DP. btdisk decodes the phone's AAC stream with it."
HOMEPAGE = "https://www.iis.fraunhofer.de/en/ff/amm/impl.html"

# From meta-openembedded's meta-multimedia (scarthgap), which the image doesn't otherwise need.
LICENSE = "Fraunhofer_FDK_AAC_Codec_Library_for_Android"
LICENSE_FLAGS = "commercial"
LIC_FILES_CHKSUM = "file://NOTICE;md5=5985e1e12f4afa710d64ed7bfd291875"

SRC_URI = "git://github.com/mstorsjo/fdk-aac.git;protocol=https;branch=master"
# v2.0.2
SRCREV = "801f67f671929311e0c9952c5f92d6e147c7b003"
S = "${WORKDIR}/git"

inherit autotools
