# libvterm 0.3.3 (MIT, Paul Evans; https://github.com/neovim/libvterm) — VT220/xterm terminal
# emulation for the Terminal app. Vendored unmodified except src/encoding.c (see the note there).
idf_component_register(
    SRCS
        "src/encoding.c"
        "src/keyboard.c"
        "src/mouse.c"
        "src/parser.c"
        "src/pen.c"
        "src/screen.c"
        "src/state.c"
        "src/unicode.c"
        "src/vterm.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu99 -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
    -Wno-missing-field-initializers -Wno-implicit-fallthrough -Wno-maybe-uninitialized
    -Wno-empty-body -Wno-error)
