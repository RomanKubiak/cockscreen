SUMMARY = "Framebuffer copy to ILI9341 SPI displays"
HOMEPAGE = "https://github.com/juj/fbcp-ili9341"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=e07269cd84249a454c5d152cf5176dd5"

inherit cmake externalsrc update-rc.d

DEPENDS = "userland"

EXTERNALSRC = "${TOPDIR}/../sources/fbcp-ili9341"
EXTERNALSRC_BUILD = "${WORKDIR}/build"

EXTRA_OECMAKE += " \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DILI9341=ON \
    -DGPIO_TFT_DATA_CONTROL=25 \
    -DGPIO_TFT_RESET_PIN=24 \
    -DGPIO_TFT_BACKLIGHT=18 \
    -DSPI_BUS_CLOCK_DIVISOR=6 \
    -DSTATISTICS=OFF \
    -DDISPLAY_ROTATE_270_DEGREES=ON \
"

INITSCRIPT_NAME = "fbcp-ili9341"
INITSCRIPT_PARAMS = "defaults 80"

FILES:${PN} += " \
    /etc/default/spi-display \
    /etc/init.d/fbcp-ili9341 \
    /etc/init.d/spi-display \
"

RDEPENDS:${PN} += "bash"

S = "${EXTERNALSRC}"

do_configure:prepend() {
    if [ -d ${B} ]; then
        find ${B} -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    fi

        python3 <<'PY'
from pathlib import Path

cmakelists = Path("${S}") / "CMakeLists.txt"
old = 'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -marm -mabi=aapcs-linux -mhard-float -mfloat-abi=hard -mlittle-endian -mtls-dialect=gnu2 -funsafe-math-optimizations")'
new = '''if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mlittle-endian -mtls-dialect=desc -funsafe-math-optimizations")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -marm -mabi=aapcs-linux -mhard-float -mfloat-abi=hard -mlittle-endian -mtls-dialect=gnu2 -funsafe-math-optimizations")
endif()'''

text = cmakelists.read_text()
if old in text:
    text = text.replace(old, new, 1)

old_link = 'target_link_libraries(fbcp-ili9341 pthread bcm_host atomic)'
new_link = 'target_link_libraries(fbcp-ili9341 pthread bcm_host vchostif atomic)'
if old_link in text:
    text = text.replace(old_link, new_link, 1)

cmakelists.write_text(text)
PY
}

do_install() {
    install -d ${D}${bindir} ${D}${sysconfdir}/init.d ${D}${sysconfdir}/default
    install -m 0755 ${B}/fbcp-ili9341 ${D}${bindir}/fbcp-ili9341
    install -m 0755 ${THISDIR}/files/fbcp-ili9341.init ${D}${sysconfdir}/init.d/fbcp-ili9341
    install -m 0755 ${THISDIR}/files/spi-display.init ${D}${sysconfdir}/init.d/spi-display
    install -m 0644 ${THISDIR}/files/spi-display.default ${D}${sysconfdir}/default/spi-display
}
