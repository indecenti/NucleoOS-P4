idf_component_register(
    SRCS
        "nv_font_14.c"
        "nv_font_20.c"
        "nv_font_28.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
)

# The generated font sources use the LV_LVGL_H_INCLUDE_SIMPLE include style (#include "lvgl.h").
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
