// sheets_skill.h — the ANIMA skill Fogli installs on the card (/sdcard/data/anima/skills/fogli.md,
// see nucleo_anima_skills.c for the format). Kept identical to sd/data/anima/skills/fogli.md, which
// the next ANIMA content pack carries. Included once, by sheets_app.cpp.
#pragma once

static const char kFogliSkill[] =
"---\n"
"name: fogli\n"
"description: spreadsheets - open, read, make and explain Excel / LibreOffice / CSV tables with the Fogli app\n"
"triggers: foglio di calcolo, fogli, excel, xlsx, xls, ods, tabella, tabelle, celle, formula, crea un foglio, "
"apri il foglio, bilancio, budget, spese del mese, preventivo, inventario, spreadsheet, sheet, table, cells, "
"hoja de calculo, tableur, classeur, tabellenkalkulation\n"
"offline: Apri Fogli dalla Home: Bilancio mensile, Lista della spesa e Registro ore sono pronti da compilare; "
"dentro Fogli il pulsante ANIMA analizza le celle selezionate.\n"
"---\n"
"Fogli is the device's spreadsheet app (app id: sheets). It opens .xlsx .xls .ods .csv .tsv, saves "
".xlsx .csv .tsv, writes formulas in the user's language (=SOMMA(B2:B8) in Italian, =SUM in English), "
"and has ANIMA inside: the user selects cells and taps ANIMA to summarize, check errors, explain a "
"formula or have formulas written into the sheet.\n"
"\n"
"- Open a spreadsheet the user names: find it (`find ~ -iname '*.xlsx'` or ls), then ACT sh open PATH. "
"Spreadsheets open in Fogli; nothing else is needed.\n"
"- Open the app itself: ACT open_app sheets.\n"
"- Read numbers out of a spreadsheet to answer a question: "
"`sc-im --nocurses --quit_afterload --export_csv F.xlsx` (formulas computed; --sheet=NAME for another "
"sheet), then compute with awk on that CSV. This works only while Fogli is closed (it uses the same "
"engine); if it fails with 'busy', ask the user to close Fogli or to ask ANIMA inside Fogli.\n"
"- Make a new spreadsheet the user describes (a budget, a list, a timesheet): write it as CSV in "
"~/Documenti, first row the column titles, one record per line, numbers with '.' for decimals and no "
"thousands separators, a totals row computed by you; then open it: ACT sh open ~/Documenti/NAME.csv. "
"Tell the user it is open in Fogli, where Save as > Excel .xlsx keeps it as a workbook and where the "
"ANIMA button inside can add formulas.\n"
"- Never claim you changed an open workbook: changes to an open sheet happen in Fogli, by the user or "
"by ANIMA's own button there.\n";
