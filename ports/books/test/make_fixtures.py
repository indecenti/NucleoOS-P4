"""make_fixtures.py — the test books that are made here instead of downloaded (ports/books/test.sh):
a comic archive (CBZ) of drawn colour pages, FB2 files (UTF-8 and windows-1251, with notes, verse, a
table and a picture), Markdown and HTML. Everything is our own text and drawing.

    python ports/books/test/make_fixtures.py DIR
"""
import base64
import io
import os
import sys
import zipfile

from PIL import Image, ImageDraw


def page(n, w=900, h=1300):
    im = Image.new("RGB", (w, h), (250, 246, 236))
    d = ImageDraw.Draw(im)
    pal = [(214, 96, 58), (58, 120, 190), (90, 160, 90), (200, 160, 40), (140, 80, 170), (40, 150, 150)]
    c = pal[n % len(pal)]
    for k in range(3):   # three panels
        y0 = 40 + k * (h - 80) // 3
        d.rectangle((40, y0, w - 40, y0 + (h - 80) // 3 - 20), outline=(20, 20, 20), width=6, fill=tuple(min(255, v + 60) for v in c))
        d.ellipse((120 + k * 150, y0 + 60, 320 + k * 150, y0 + 260), fill=c, outline=(20, 20, 20), width=5)
    d.text((60, h - 34), "page %d" % (n + 1), fill=(20, 20, 20))
    b = io.BytesIO()
    im.save(b, "JPEG", quality=85)
    return b.getvalue()


def cbz(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        for n in range(8):   # names in natural order on purpose: page10 after page9
            z.writestr("pages/page%d.jpg" % (n + 1), page(n))
        z.writestr("ComicInfo.xml", "<?xml version='1.0'?><ComicInfo><Title>Test comic</Title><Writer>NucleoOS</Writer>"
                                    "<LanguageISO>en</LanguageISO><Summary>Eight drawn pages.</Summary></ComicInfo>")


def png_bytes():
    im = Image.new("RGB", (320, 200), (58, 120, 190))
    ImageDraw.Draw(im).ellipse((100, 40, 220, 160), fill=(242, 193, 78))
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


FB2 = """<?xml version="1.0" encoding="{enc}"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
<description><title-info><genre>prose</genre><author><first-name>Иван</first-name><last-name>Тестов</last-name></author>
<book-title>Проверка книги</book-title><annotation><p>Короткая книга для проверки читалки.</p></annotation>
<coverpage><image l:href="#cover.png"/></coverpage><lang>ru</lang></title-info>
<publish-info><publisher>NucleoOS</publisher><year>2026</year></publish-info></description>
<body><title><p>Проверка книги</p></title>
<section id="ch1"><title><p>Глава 1</p><p>Начало</p></title>
<epigraph><p>Эпиграф к первой главе.</p><text-author>Автор</text-author></epigraph>
<p>Первый абзац с <emphasis>курсивом</emphasis> и <strong>жирным</strong> текстом, и сноской<a l:href="#n1" type="note">1</a>.</p>
<p>Второй абзац, довольно длинный, чтобы проверить перенос строк и выравнивание по ширине страницы в читалке.</p>
<empty-line/>
<poem><stanza><v>Строка стиха первая,</v><v>строка стиха вторая.</v></stanza></poem>
<image l:href="#cover.png"/>
<table><tr><th>Имя</th><th>Значение</th></tr><tr><td>а</td><td>1</td></tr></table>
</section>
<section id="ch2"><title><p>Глава 2</p></title><p>Конец книги.</p></section>
</body>
<body name="notes"><section id="n1"><title><p>1</p></title><p>Текст сноски номер один.</p></section></body>
<binary id="cover.png" content-type="image/png">{png}</binary>
</FictionBook>
"""

MD = """# Il manuale di prova

Un paragrafo con *corsivo*, **grassetto**, `codice` e un [collegamento](https://example.org).

## Elenco

- primo punto
- secondo punto
  - punto interno

1. uno
2. due

> Una citazione, in corsivo.

```
codice preformattato
    con rientro
```

## Fine

L'ultimo paragrafo del manuale.
"""

HTML = """<!doctype html><html><head><meta charset="utf-8"><title>Pagina di prova</title></head><body>
<h1>Pagina di prova</h1><p>Un paragrafo in <em>HTML</em>.</p><h2>Seconda parte</h2><p>Altro testo.</p></body></html>
"""


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    cbz(os.path.join(out, "comic.cbz"))
    png = base64.b64encode(png_bytes()).decode()
    with open(os.path.join(out, "test-utf8.fb2"), "w", encoding="utf-8", newline="\n") as f:
        f.write(FB2.format(enc="utf-8", png=png))
    with open(os.path.join(out, "test-1251.fb2"), "w", encoding="cp1251", newline="\n") as f:
        f.write(FB2.format(enc="windows-1251", png=png))
    with zipfile.ZipFile(os.path.join(out, "test.fb2.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("test.fb2", FB2.format(enc="utf-8", png=png).encode("utf-8"))
    with open(os.path.join(out, "manuale.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(MD)
    with open(os.path.join(out, "pagina.html"), "w", encoding="utf-8", newline="\n") as f:
        f.write(HTML)
    print("fixtures made in", out)


if __name__ == "__main__":
    main()
