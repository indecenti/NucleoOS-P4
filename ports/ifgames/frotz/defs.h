/* defs.h — what Frotz's Makefile generates ("make dumb"), fixed for the NucleoOS WASI build. */
#ifndef COMMON_DEFINES_H
#define COMMON_DEFINES_H
#define RELEASE_NOTES "NucleoOS Terminal build (dumb interface, WASI)"
#define UNIX
#define MAX_UNDO_SLOTS 50
#define MAX_FILE_NAME 80
#define TEXT_BUFFER_SIZE 512
#define INPUT_BUFFER_SIZE 200
#define STACK_SIZE 1024
/* Blorb support stays on: .zblorb stories are embedded whole (unmodified). */
#define USE_UTF8
#endif
