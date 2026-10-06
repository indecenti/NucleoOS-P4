---
name: dati
description: read, analyse and change data files - CSV, JSON, logs, lists of numbers - with exact results
triggers: csv, json, log, file di testo, somma, media, conta le righe, quante righe, colonna, analizza il file, filtra, ordina, il piu' grande, il piu' vecchio, statistiche, trova nel file, cerca nel file, cambia il valore, modifica il file, average, sum, count lines, column, filter, sort, parse, analyse the file, change the value
---
The answer is in the file, not in your head: read it, then compute with a tool and report its output.

1) Look first (one call): `wc -l F; head -5 F` (CSV: header + a few rows; JSON: `jq . F | head -30`).
2) Compute exactly, never by eye:
   - numbers one per line: `awk '{s+=$1} END {print s, NR, s/NR}' F` (sum, count, average)
   - CSV column N (header skipped): `awk -F, 'NR>1 {s+=$N; n++} END {print s/n}' F`
   - JSON array: `jq 'length' F`, `jq '[.[] | select(.eta > 30)] | length' F`,
     `jq -r 'max_by(.eta) | .nome' F`, `jq -r '.[] | "\(.nome) \(.eta)"' F`
   - logs: `grep -n ERROR F`, `grep -c WARN F`, `grep '14:3' F | head`
   - anything longer: a lua or python script (see the python skill), printing the result.
3) Change a file with edit_file (old text exactly as read -> new text), never by rewriting it from
   memory; for JSON you may also `jq '.volume = 55' F > F.tmp` then `mv F.tmp F`. Read it back
   (`cat F`) and say what changed.
4) Reply with the number or the rows found, plus one line on how (the command), in the user's language.
Words in a request about a file ("volume", "media", "più di 30") describe the DATA: they are not
device settings or a sum to work out.
