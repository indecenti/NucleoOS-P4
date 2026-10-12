# sc-im, hoja de cálculo

> **La hoja de cálculo para tacto, ratón y ANIMA es la app Hojas** (en Inicio): sc-im es su motor de cálculo y viene con el sistema. Esta guía es para quien prefiere sc-im con el teclado, en la Terminal.

## Para qué sirve

sc-im es una hoja de cálculo de verdad, a pantalla completa: celdas con números, textos y fórmulas que se recalculan solas, funciones (`@sum`, `@avg`, `@max`, `@if`, fechas y horas), formatos de número, ancho de columnas, ordenación, filtros, deshacer y rehacer. Abre archivos **Excel `.xlsx` y `.xls`**, **LibreOffice `.ods`**, **`.csv`** y **`.tsv`**, y guarda en `.xlsx`, `.csv` y `.tsv`: una lista de gastos, un inventario, un plan de pagos, una hoja que te enviaron por correo.

## Cómo se abre

- **Toca el icono** en Inicio: se abre una hoja vacía.
- En **Archivos**, toca un `.xlsx`, `.xls`, `.ods` o `.csv` y elige **Abrir con › sc-im**.
- En el Terminal: `sc-im gastos.xlsx`.

## Tacto y teclado

- **Toca una celda** para seleccionarla. **Arrastra arriba y abajo** para desplazar la hoja.
- La fila de teclas sobre el teclado tiene **Esc**, **Tab**, **Ctrl** y las **flechas**.
- Con un teclado USB o KeyDeck también funcionan Ctrl, Shift y las teclas de función.

## Los comandos principales

sc-im se usa como el editor vim: una letra es un comando, los comandos largos empiezan con dos puntos.

| Teclas | Qué hacen |
|---|---|
| flechas | mueven la selección |
| `=` | escribe un número o una fórmula, p. ej. `=@sum(B0:B9)` y luego Intro |
| `<` `>` `\` | escribe un texto alineado a la izquierda, a la derecha, centrado |
| `e` / `E` | edita el número / el texto de la celda |
| `x` | borra la celda |
| `u` / Ctrl+R | deshacer / rehacer |
| `yy` y luego `p` | copia la celda y pégala |
| `ir` `ic` | inserta una fila / una columna |
| `dr` `dc` | elimina la fila / la columna |
| `f>` `f<` | columna más ancha / más estrecha |
| `f+` `f-` | más / menos decimales |
| `goB12` | ve a la celda B12 |
| `:w` | guarda; `:w cuentas.xlsx` guarda como Excel |
| `:e csv cuentas.csv` | exporta a CSV (también `tab`, `txt`, `mkd`) |
| `:load archivo.ods` | abre otro archivo |
| `:q` | sale (`:q!` sin guardar) |
| `:h` | el manual completo (`q` lo cierra) |

Las celdas se nombran con la columna y la fila, **contando las filas desde 0**: `A0` es la primera celda arriba a la izquierda. En las fórmulas los rangos se escriben `A0:A9`.

## Con Anima

sc-im es un programa del sistema: Anima lo usa para leer tus hojas. Pregunta por ejemplo "¿cuánto suma el total de gastos.xlsx?" o "resume la tabla pedidos.ods".

## Límites

- Los gráficos, macros, imágenes y comentarios de los archivos de Excel no se leen; de un `.xlsx` se leen valores, textos y (con la opción `--xlsx_readformulas`) fórmulas.
- Los archivos `.ods` se abren pero se guardan en `.xlsx`, `.csv` o en el formato de sc-im (`.sc`).
- De un `.xls` antiguo se lee la primera hoja; se guarda en `.xlsx`.
- El portapapeles del sistema y los comandos `!` (programas externos) no están disponibles.
- Las hojas muy grandes (decenas de miles de filas) pueden no caber en la memoria del dispositivo: sc-im lo avisa y el archivo original queda intacto, porque primero guarda en un archivo temporal.

## En el Terminal

- **Toca el icono** para abrir el Terminal con el programa ya iniciado, o abre **Terminal** y escribe el comando.
- Tus archivos están en la carpeta **home** de la tarjeta SD (`/sdcard/home`): para el programa esa carpeta es `/`. Puedes copiar archivos desde el PC poniendo la SD en el lector.
- Leer una hoja sin abrirla: `sc-im --nocurses --quit_afterload --export_csv gastos.xlsx` (con `--export_mkd` sale una tabla).
- Con `apps` el Terminal lista los programas instalados, con `help sc-im` la sintaxis.

---

*This product includes software developed by Andrés Martinelli <andmarti@gmail.com>.*
