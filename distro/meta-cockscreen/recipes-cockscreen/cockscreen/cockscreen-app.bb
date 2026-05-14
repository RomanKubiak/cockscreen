SUMMARY = "Cockscreen realtime shader pipeline"
LICENSE = "CLOSED"
LIC_FILES_CHKSUM = ""

inherit qt6-cmake pkgconfig externalsrc update-rc.d

DEPENDS = " \
    alsa-lib \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    libdrm \
    qtbase \
    qtmultimedia \
"

EXTERNALSRC = "${TOPDIR}/../.."
EXTERNALSRC_BUILD = "${WORKDIR}/build"

EXTRA_OECMAKE += " \
    -DCOCKSCREEN_ENABLE_STRICT_WARNINGS=OFF \
    -DCOCKSCREEN_TARGET_PLATFORM=raspberry-pi-zero-2w \
    -DFETCHCONTENT_SOURCE_DIR_PFFFT=${TOPDIR}/../sources/pffft \
"

INITSCRIPT_NAME = "cockscreen"
INITSCRIPT_PARAMS = "defaults 95"

RDEPENDS:${PN} += " \
    gstreamer1.0 \
    gstreamer1.0-libav \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    qtbase \
    qtmultimedia \
"

RPROVIDES:${PN} += "cockscreen"

S = "${EXTERNALSRC}"

python create_source_date_epoch_stamp() {
    import oe.reproducible

    source_dir = d.getVar('S')
    source_date_epoch = oe.reproducible.get_source_date_epoch_from_git(d, source_dir)
    if not source_date_epoch:
        source_date_epoch = oe.reproducible.get_source_date_epoch_from_known_files(d, source_dir)
    if not source_date_epoch:
        source_date_epoch = oe.reproducible.fixed_source_date_epoch(d)

    oe.reproducible.epochfile_write(source_date_epoch, d.getVar('SDE_FILE'), d)
}

do_create_recipe_spdx[noexec] = "1"
do_create_spdx[noexec] = "1"
do_create_package_spdx[noexec] = "1"
INHIBIT_PACKAGE_DEBUG_SPLIT = "1"
NOAUTOPACKAGEDEBUG = "1"
PACKAGE_DEBUG_SPLIT_STYLE = "debug-without-src"
PACKAGES = "${PN}"
INSANE_SKIP:${PN} += "installed-vs-shipped debug-files"
do_package[cleandirs] += " ${WORKDIR}/pkgdata-sysroot ${PKGDEST} ${PKGD}"

FILES:${PN} = " /usr /etc /opt "

do_install() {
    install -d ${D}/opt/cockscreen
    install -m 0755 ${B}/cockscreen ${D}/opt/cockscreen/cockscreen
    cp -R --no-dereference --preserve=mode,timestamps ${S}/resources ${D}/opt/cockscreen/
    cp -R --no-dereference --preserve=mode,timestamps ${S}/scenes ${D}/opt/cockscreen/
    cp -R --no-dereference --preserve=mode,timestamps ${S}/shaders ${D}/opt/cockscreen/
    install -d ${D}${sysconfdir}/default ${D}${sysconfdir}/init.d ${D}${bindir}
    install -m 0644 ${THISDIR}/files/cockscreen.default ${D}${sysconfdir}/default/cockscreen
    install -m 0755 ${THISDIR}/files/cockscreen.init ${D}${sysconfdir}/init.d/cockscreen
    ln -sf /opt/cockscreen/cockscreen ${D}${bindir}/cockscreen
}