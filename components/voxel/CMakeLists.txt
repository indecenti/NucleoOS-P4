# voxel — the VoxelSpaceNG terrain engine (github.com/indecenti/voxelspace-esp32), vendored by
# platform/nucleo/sync_firmware.ps1 of that repository: core/ is the portable renderer, voxel_nv.c
# the NucleoOS platform (both cores, SRAM strips + AXI-GDMA, files, adaptive quality).
idf_component_register(
    SRCS "voxel_nv.c" "core/vsng.c" "core/vsng_mt.c"
    INCLUDE_DIRS "." "core"
    PRIV_REQUIRES esp_timer heap freertos log nv_kernel nv_hal
)

# Pure CPU work: -O3 with unrolled loops (the per-step march and the span fills are the frame).
# Per-section cycle counters exist (-DVS_CYCLES) but esp_cpu_get_cycle_count proved unreliable for them.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -funroll-loops)