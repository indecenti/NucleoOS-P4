# WordGrinder, procesador de textos

## Para qué sirve

WordGrinder es un programa para **escribir**: cartas, informes, apuntes, capítulos. Te deja concentrarte en el texto, sin distracciones, pero tiene lo necesario: títulos, párrafos, listas con viñetas y numeradas, **negrita**, *cursiva*, subrayado, buscar y reemplazar, recuento de palabras, deshacer y rehacer, varios documentos en el mismo archivo. Abre y exporta **OpenDocument `.odt`** (LibreOffice; Word también lo abre), **Markdown**, **HTML** y texto; también exporta LaTeX, troff y Org.

## Cómo se abre

- **Toca el icono** en Inicio: se abre un documento vacío.
- En **Archivos**, toca un `.odt`, `.md`, `.html` o `.txt` y elige **Abrir con › WordGrinder**.
- En el Terminal: `wordgrinder carta.odt`.

Un `.odt`, `.md`, `.html` o `.txt` se **importa**: al guardar, WordGrinder escribe un archivo `.wg` al lado (su formato) y el original no se toca. Para volver a escribirlo en `.odt` usa **ESC › File › Export**.

## Tacto y teclado

- **Toca el texto** para llevar el cursor allí. **Arrastra arriba y abajo** para desplazarte.
- **ESC** (en la fila de teclas sobre el teclado) abre el **menú**: File, Edit, Style, Documents, Navigation. Cada opción tiene una letra, y las más usadas tienen un atajo con Ctrl.
- Con un teclado USB o KeyDeck también funcionan Mayús+flechas (selección) y Ctrl+flechas (palabra a palabra).

| Teclas | Qué hacen |
|---|---|
| ESC | menú |
| Ctrl+S | guarda |
| Ctrl+O | abre un documento |
| Ctrl+B / Ctrl+U | negrita / subrayado (cursiva: ESC › Style › Set italic) |
| Ctrl+N | texto normal |
| Ctrl+P | estilo del párrafo: títulos H1–H4, listas, citas |
| Ctrl+Z / Ctrl+Y | deshacer / rehacer |
| Ctrl+X / Ctrl+C / Ctrl+V | cortar / copiar / pegar |
| Ctrl+F / Ctrl+K | buscar y reemplazar / buscar siguiente |
| Ctrl+G | ir a una página o a un título |
| Ctrl+Q | salir |

Los menús de WordGrinder están en inglés.

## Con Anima

WordGrinder es un programa del sistema: Anima lo usa para leer un `.odt` o convertir apuntes en un documento. Pregunta por ejemplo "léeme el archivo informe.odt" o "convierte notas.md en un documento odt".

## Límites

- Los archivos de Word `.docx` y `.doc` no se abren: guárdalos como `.odt`.
- Nada de imágenes, tablas, columnas ni maquetación: es un programa para texto. Las imágenes de un `.odt` importado se pierden.
- El corrector ortográfico no tiene diccionarios instalados.
- El portapapeles es interno del programa.

## En el Terminal

- **Toca el icono** para abrir el Terminal con el programa ya iniciado, o abre **Terminal** y escribe el comando.
- Tus archivos están en la carpeta **home** de la tarjeta SD (`/sdcard/home`): para el programa esa carpeta es `/`. Puedes copiar archivos desde el PC poniendo la SD en el lector.
- Convertir sin abrir: `wordgrinder --convert carta.odt carta.txt` (la extensión decide el formato: `.odt` `.md` `.html` `.txt` `.wg`, de salida también `.tex`).
- Con `apps` el Terminal lista los programas instalados, con `help wordgrinder` la sintaxis.
