# components/luavgl — lib/luavgl/src for the P4. luavgl is a unity build:
# luavgl.c #includes every other .c under src/ (anim, constants, disp, font,
# fs, group, indev, obj -> event/style/widgets/widgets.c -> each widget,
# palette, timer, util, rotable), so luavgl.c is the one translation unit.
# PlatformIO also compiles the other files on the S3 boards, where their
# calls into luavgl.c's static helpers are implicit declarations gcc 8 only
# warns about, and those objects are never linked; the P4's gcc 14 rejects
# implicit declarations, so only luavgl.c is compiled here.
set(LUAVGL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/luavgl/src")
idf_component_register(
    SRCS "${LUAVGL_DIR}/luavgl.c"
    INCLUDE_DIRS "${LUAVGL_DIR}"
    REQUIRES lua lvgl)
