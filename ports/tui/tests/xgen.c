// xgen.c — test workbooks for the sc-im xlsx importer (built with libxlsxwriter).
//   OUT=/mix.xlsx  MODE=mix   two sheets: types, dates, times, booleans, formulas
//   OUT=/inl.xlsx  MODE=inl   constant_memory (inline strings)
#include "xlsxwriter.h"
#include <stdlib.h>
#include <string.h>
int main(void) {
    const char *mode = getenv("MODE");
    lxw_workbook_options opt = {0};
    opt.constant_memory = !strcmp(mode, "inl");
    opt.tmpdir = "/tmp";
    lxw_workbook *wb = workbook_new_opt(getenv("OUT"), &opt);
    lxw_worksheet *a = workbook_add_worksheet(wb, "Spese \"casa\"");
    lxw_format *date = workbook_add_format(wb);
    format_set_num_format(date, "dd/mm/yyyy");
    lxw_format *time = workbook_add_format(wb);
    format_set_num_format(time, "hh:mm:ss");
    worksheet_write_string(a, 0, 0, "voce", NULL);
    worksheet_write_string(a, 0, 1, "importo", NULL);
    worksheet_write_string(a, 0, 2, "data", NULL);
    worksheet_write_string(a, 1, 0, "affitto \"giugno\" àè", NULL);
    worksheet_write_number(a, 1, 1, 650.5, NULL);
    lxw_datetime d = {2026, 6, 15, 0, 0, 0};
    worksheet_write_datetime(a, 1, 2, &d, date);
    worksheet_write_string(a, 2, 0, "luce", NULL);
    worksheet_write_number(a, 2, 1, 80.25, NULL);
    lxw_datetime t = {0, 0, 0, 13, 30, 15};
    worksheet_write_datetime(a, 2, 2, &t, time);
    worksheet_write_string(a, 3, 0, "pagato", NULL);
    worksheet_write_boolean(a, 3, 1, 1, NULL);
    worksheet_write_string(a, 4, 0, "totale", NULL);
    worksheet_write_formula_num(a, 4, 1, "=SUM(B2:B3)", NULL, 730.75);
    if (strcmp(mode, "inl")) {
        lxw_worksheet *b = workbook_add_worksheet(wb, "Secondo");
        worksheet_write_string(b, 0, 0, "x", NULL);
        worksheet_write_number(b, 0, 1, 42, NULL);
    }
    return workbook_close(wb);
}
