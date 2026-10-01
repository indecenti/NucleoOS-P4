// time extras for NucleoOS (included by extmod/modtime.c): wall clock from WASI clock_gettime.
#include <time.h>
#include "py/obj.h"
#include "shared/timeutils/timeutils.h"

static mp_obj_t mp_time_localtime_get(void) {
    timeutils_struct_time_t tm;
    timeutils_seconds_since_epoch_to_struct_time(mp_hal_time_ns() / 1000000000ULL, &tm);
    mp_obj_t tuple[8] = {
        mp_obj_new_int(tm.tm_year), mp_obj_new_int(tm.tm_mon), mp_obj_new_int(tm.tm_mday),
        mp_obj_new_int(tm.tm_hour), mp_obj_new_int(tm.tm_min), mp_obj_new_int(tm.tm_sec),
        mp_obj_new_int(tm.tm_wday), mp_obj_new_int(tm.tm_yday),
    };
    return mp_obj_new_tuple(8, tuple);
}

static mp_obj_t mp_time_time_get(void) {
    return mp_obj_new_float((mp_float_t)mp_hal_time_ns() / 1e9);
}
