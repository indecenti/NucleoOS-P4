# Hyphenation patterns of the Libri app

The files here are the unmodified sources the app's hyphenation data is made from
(`ports/books/gen_hyph.py` writes `hyphdata.c` from them). Each file keeps its own copyright and
license notice in its header; the notice text is also shown in the app's third-party notices.

| App language | File | Source | License used |
|---|---|---|---|
| it | hyph-it.tex | TeX hyph-utf8 | MIT (the file offers LPPL 1.3 or MIT), (c) Claudio Beccari |
| en | hyph-en-us.tex | TeX hyph-utf8 | permissive terms in the header (copyright notice kept), (c) Gerard D.C. Kuiken |
| es | hyph-es.tex | TeX hyph-utf8 | MIT/X11, (c) Javier Bezos |
| fr | hyph-fr.tex | TeX hyph-utf8 | MIT, (c) Flipo, Gaulle, Reutenauer |
| de | hyph-de-1996.tex | TeX hyph-utf8 | MIT, (c) Stephan Hennig, Werner Lemberg and contributors |
| pt | hyph-pt.tex | TeX hyph-utf8 | BSD 3-clause, (c) de Rezende, Dias Almeida, Araujo, Benevides |
| nl | hyph-nl.tex | TeX hyph-utf8 | MIT, (c) Piet Tutelaers |
| da | hyph-da.tex | TeX hyph-utf8 | MIT (the file offers LPPL 1.3 or MIT), (c) Frank Jensen |
| pl | hyph-pl.tex | TeX hyph-utf8 | MIT (the file offers MIT or Knuth's terms), (c) Kolodziejska, Jackowski, Rycko |
| la | hyph-la.tex | TeX hyph-utf8 | MIT (the file offers MIT or LPPL), (c) Claudio Beccari |
| sv | hyph_sv.dic | LibreOffice dictionaries | MPL (the file is MPL / LGPLv3+), (c) Niklas Johansson; README_hyph_sv.txt |

Sources: https://github.com/hyphenation/tex-hyphen (hyph-utf8/tex/generic/hyph-utf8/patterns/tex/)
and https://github.com/LibreOffice/dictionaries (sv_SE). Not included on purpose: Hungarian (size),
Russian and Catalan (LPPL only), Finnish (no permission to convert), Czech (GPL).
