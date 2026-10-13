# Capture the Pi's local wall-clock time at configure time so the watch's
# RTC can be seeded with a sensible time right after flashing.
string(TIMESTAMP BUILD_STAMP "%Y-%m-%d %H:%M:%S")

# Home-face photos: raw 410x502 RGB565 files produced by tools/make_pics.py,
# embedded straight into the app image (read from flash, no RAM needed).
file(GLOB PIC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/pics/pic_*.bin)
list(SORT PIC_FILES)
list(LENGTH PIC_FILES PIC_COUNT)

idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    EMBED_FILES ${PIC_FILES})

target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_STAMP="${BUILD_STAMP}" PIC_COUNT=${PIC_COUNT})
