SUMMARY = "Minimal cockscreen image for Raspberry Pi Zero 2 W"
LICENSE = "MIT"

inherit core-image

do_create_rootfs_spdx[noexec] = "1"
do_create_image_spdx[noexec] = "1"
do_create_image_sbom_spdx[noexec] = "1"

IMAGE_FEATURES = ""
IMAGE_INSTALL:append = " \
    alsa-utils \
    cockscreen-app \
    fbcp-ili9341 \
    i2c-tools \
    kernel-modules \
    spitools \
"
