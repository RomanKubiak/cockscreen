PACKAGECONFIG:append = " gui widgets fontconfig harfbuzz png jpeg gles2 eglfs kms gbm"
PACKAGECONFIG:remove = " linuxfb xcb wayland vulkan"
QT_QPA_DEFAULT_PLATFORM = "eglfs"
