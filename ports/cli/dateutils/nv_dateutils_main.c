// nv_dateutils_main.c — dateutils for the NucleoOS Terminal (WASI) as one multi-call program.
//
// The shell runs `dateadd ARGS` as `dateutils dateadd ARGS` (term_sh's table of system multi-call
// commands), so argv[1] names the tool. Each upstream program is compiled with -Dmain=<x>_main.
// datesort is left out (it forks sort and cut).
#include <stdio.h>
#include <string.h>

int dadd_main(int, char **);
int ddiff_main(int, char **);
int dseq_main(int, char **);
int dconv_main(int, char **);
int dround_main(int, char **);
int dtest_main(int, char **);
int dgrep_main(int, char **);
int dzone_main(int, char **);
int strptime_main(int, char **);

// dt-io.c prints error messages under this name; each program's own `prog` is renamed (build_wsl.sh)
const char *prog = "dateutils";

static const struct { const char *name; int (*fn)(int, char **); const char *what; } kTools[] = {
    {"dateadd", dadd_main, "add durations to dates: dateadd 2026-10-02 +45d, +3w, +10b (business days)"},
    {"datediff", ddiff_main, "difference between dates: datediff 2026-10-02 2026-12-25 [-f '%w weeks %d days']"},
    {"dateseq", dseq_main, "sequences of dates: dateseq 2026-10-01 +1w 2026-11-01"},
    {"dateconv", dconv_main, "convert date formats: dateconv 2026-10-02 -f '%A %d %B %Y'"},
    {"dateround", dround_main, "round dates to the next weekday or unit: dateround 2026-10-02 Mon"},
    {"datetest", dtest_main, "compare dates: datetest 2026-10-02 --lt 2026-12-25"},
    {"dategrep", dgrep_main, "lines whose dates match: dategrep '>=2026-10-01' < FILE"},
    {"datezone", dzone_main, "a date in several time zones (needs zoneinfo on the SD card)"},
    {"strptime", strptime_main, "parse dates in any format: strptime -i '%d/%m/%Y' 02/10/2026"},
};

int main(int argc, char **argv) {
    if (argc >= 2) {
        const char *t = argv[1];
        if (!strncmp(t, "dateutils.", 10)) t += 10;
        for (size_t i = 0; i < sizeof kTools / sizeof kTools[0]; i++) {
            const char *n = kTools[i].name;
            // also the short upstream names (dadd, ddiff, ...): "date" + rest
            if (!strcmp(t, n) || (t[0] == 'd' && !strcmp(t + 1, n + 4))) {
                argv[1] = (char *)kTools[i].name;
                prog = kTools[i].name;
                return kTools[i].fn(argc - 1, argv + 1);
            }
        }
    }
    printf("dateutils 0.4.12 - date and time arithmetic. Tools:\n");
    for (size_t i = 0; i < sizeof kTools / sizeof kTools[0]; i++)
        printf("  %-10s %s\n", kTools[i].name, kTools[i].what);
    printf("Each takes --help. Dates are ISO (2026-10-02) unless -i FORMAT says otherwise.\n");
    return argc >= 2 ? 1 : 0;
}
