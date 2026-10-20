# DMD Flipper — Guía del usuario

**DMD Flipper** convierte tu placa NucleoOS (ESP32-P4 con pantalla táctil de 1024x600) en el DMD de tu
máquina de pinball virtual y, cuando no hay ninguna mesa en marcha, en un menú táctil de tus mesas de
PinUP Popper. En la interfaz en español la app se llama **DMD Pinball**.

Esta guía es para el propietario de la máquina: explica cómo conectar la placa, cómo instalar el pequeño
programa "puente" en el PC y cómo usar cada pantalla. No hace falta saber programar.

![El reloj de puntos cuando el PC calla](../../screenshots/dmd_clock.jpg)

---

## Índice

1. [Qué es y qué necesitas](#1-qué-es-y-qué-necesitas)
2. [Inicio rápido en 5 pasos](#2-inicio-rápido-en-5-pasos)
3. [Conectar el PC](#3-conectar-el-pc)
4. [Instalar el puente para PinUP Popper](#4-instalar-el-puente-para-pinup-popper)
5. [El menú de mesas](#5-el-menú-de-mesas)
6. [Durante la partida](#6-durante-la-partida)
7. [El panel de ajustes](#7-el-panel-de-ajustes)
8. [La pantalla Composición](#8-la-pantalla-composición)
9. [Solución de problemas](#9-solución-de-problemas)
10. [Actualizar el firmware con seguridad](#10-actualizar-el-firmware-con-seguridad)
11. [Límites conocidos](#11-límites-conocidos)
12. [Privacidad y red](#12-privacidad-y-red)

---

## 1. Qué es y qué necesitas

La app hace dos trabajos:

- **El DMD de la máquina.** Durante una partida el PC (Visual Pinball X + VPinMAME + dmdext) envía el DMD a la
  placa, que lo dibuja como puntos LED/plasma. Los fotogramas pueden llegar por **USB** o por **Wi-Fi**, o por
  los dos a la vez.
- **Un menú táctil para PinUP Popper.** Cuando no hay ninguna mesa en marcha, la placa muestra tus mesas
  (recientes, nuevas, más jugadas, favoritas, todas) con wheels, backglass, estadísticas y puntuaciones: tocas
  **Jugar** y la mesa arranca en la máquina. Para esto hace falta el **puente**, un pequeño programa que se
  ejecuta en el PC (capítulo 4).

### Qué necesitas

| Qué | Detalles |
|---|---|
| Placa NucleoOS | Con la app **DMD Pinball**. La microSD es opcional: si la hay, la placa guarda en ella las imágenes y las últimas listas y el menú se vuelve instantáneo; sin ella el menú funciona igual, pero las imágenes llegan por el cable cada vez y no se conserva nada entre sesiones. |
| Cable USB | Desde el puerto USB de programación de la placa (Windows lo ve como dispositivo USB 303A:1001) **directamente al PC**. Nada de hubs sin alimentación: la placa se quedaría sin corriente y se reiniciaría. |
| PC de la máquina | Windows 10 (versión 1803 o posterior) u 11, Visual Pinball X, VPinMAME con **dmdext 2.x** (su `DmdDevice.dll`). |
| PinUP Popper | Para el menú de mesas, con su **Web Remote** activado (capítulo 4). |
| Python | 3.8 o posterior, para el puente. |
| Wi-Fi | Opcional para el DMD (basta el USB), **recomendado** con Popper: sirve para salir de una mesa VPX o iniciar otra desde la placa mientras se juega. |

---

## 2. Inicio rápido en 5 pasos

1. **Conecta** la placa al PC con el cable USB, directamente (sin hubs sin alimentación).
2. **Abre DMD Pinball** en la placa. Mantén pulsada la pantalla 2 segundos para abrir el panel y activa
   **Iniciar al encender**: la app debe estar ya abierta cuando arranca una mesa.
3. **En el PC**, en el `DmdDevice.ini` de dmdext, activa la salida USB:
   ```ini
   [zedmd]
   enabled = true
   ```
4. **Instala el puente** para Popper (capítulo 4): copia la carpeta, `pip install pyserial pillow`, añade las
   dos líneas a los scripts de los emuladores en Popper, y luego
   `powershell -ExecutionPolicy Bypass -File ponte.ps1 installa`.
5. **Pruébalo**: sin ninguna mesa en marcha la placa muestra el menú. Elige una mesa, toca **Jugar**: la mesa
   arranca y la placa pasa al DMD. Sal de la mesa: vuelve el menú.

---

## 3. Conectar el PC

La placa recibe el DMD de dos maneras, activas a la vez. Se elige en el `DmdDevice.ini` de dmdext en el PC (el
archivo junto al `DmdDevice.dll` de VPinMAME). La página de espera de la app muestra las dos configuraciones,
con la dirección exacta de tu placa.

### USB (recomendado)

```ini
[zedmd]
enabled = true
```

- La placa se presenta a dmdext como un **ZeDMD 128x32**: dmdext busca entre los puertos serie y la encuentra
  solo. Para fijar el puerto, añade `port = COMx` bajo `[zedmd]` (el número de puerto COM de la placa en el
  Administrador de dispositivos; `ponte.ps1 stato` lo muestra en la línea "Scheda").
- **La app debe estar abierta cuando arranca la mesa**: dmdext busca el dispositivo en ese momento. Por eso
  conviene **Iniciar al encender** (más abajo).
- Los DMD de otros tamaños (por ejemplo 128x16 o 192x64) los adapta dmdext a 128x32.
- Con el puente para Popper, el USB es también el canal del menú: el puente y dmdext se pasan el puerto al
  empezar y al terminar cada partida, solos.

### Wi-Fi

```ini
[networkstream]
enabled = true
url = ws://<IP de la placa>:9090/dmd
retry = true
```

- La dirección exacta (`ws://…:9090/dmd`) aparece en la página de espera de la app y en la parte superior del
  panel de ajustes, bajo la línea de estado.
- Por Wi-Fi la placa muestra el DMD a su tamaño real, hasta 512x256.
- Un PC cada vez: una conexión nueva sustituye a la anterior.
- El puerto 9090 se puede cambiar en el panel (**Puerto de red**); con `retry = true` el PC se vuelve a
  conectar solo.

> **Consejo — IP fija.** En el router, dale a la placa una dirección fija (reserva DHCP): así la URL del
> `DmdDevice.ini` no cambia nunca.

### Iniciar al encender

En el panel (sección **Sistema**) activa **Iniciar al encender** (desactivado de fábrica): un segundo después
del encendido, NucleoOS abre directamente DMD Pinball. Si la pantalla de bloqueo está activa, la app no se abre
sola hasta que desbloquees: en una máquina conviene no usar el bloqueo.

---

## 4. Instalar el puente para PinUP Popper

### Qué hace

El puente (`nucleo_bridge.py`) se ejecuta en el PC de la máquina, arranca con Windows y apenas pesa (prioridad
baja, unos 27 MB, sin ventana). Todo el trabajo gráfico lo hace la placa.

- **Sin mesa en marcha** envía a la placa las listas de mesas, las fichas de las mesas y las imágenes
  (convertidas una sola vez y guardadas en caché).
- **Cuando arranca una mesa VPX** cede el puerto USB a dmdext: la placa hace de DMD. Al final de la partida lo
  recupera.
- **Mesas de Future Pinball con DMD**: el puente lo copia a la placa con el modo mirror de dmdext.
- **Pinball FX, MAME, juegos de PC**: el puente se queda con el puerto y la placa muestra el escaparate del
  juego en curso, con las acciones rápidas.
- **Iniciar y salir** de las mesas pasa por el Web Remote de Popper.
- Lee la base de datos de Popper (`PUPDatabase.db`) **solo en lectura**: nunca escribe nada en ella. Las mesas
  ocultas en Popper no aparecen.
- Si Popper no avisa del final de un juego (un emulador que se colgó o se cerró a la fuerza), el puente lo nota
  solo cuando termina el proceso del emulador, y vuelve el menú.
- Cierra el servidor del backglass B2S que VPX a veces deja abierto tras un cuelgue (si no, Popper ya no
  iniciaría nada).
- Se puede reiniciar incluso durante una partida: reconoce la mesa en marcha.

### Requisitos

- Python 3.8 o posterior, con `pip install pyserial pillow` (numpy es opcional: acelera las imágenes).
- PinUP Popper, con el **Web Remote** activado:
  - `useWEB = true ;` en `PUPMenuScriptSysOptions.txt` y en `PuPMenuScript.pup` (las "opciones de script" de
    Popper), y luego reinicia Popper;
  - el componente COM `PuPServer.WebServer` registrado (si no lo está: `PUPServerRegisterCOM.bat` como
    administrador);
  - puerto 80 (si Popper usa otro puerto, inicia el puente con `--web-port`).
  El Web Remote es necesario para **Jugar**, para salir de un juego y para el escaparate.
- dmdext con `[zedmd] enabled = true` en el `DmdDevice.ini`.
- En la placa: la app **DMD Pinball** abierta (mejor con **Iniciar al encender**).

### Instalación

1. Copia la carpeta `popper_bridge` al PC, por ejemplo en `C:\vPinball\nucleo_bridge`.
2. Instala las bibliotecas:
   ```
   pip install pyserial pillow
   ```
3. Comprueba que lo encuentra todo:
   ```
   python nucleo_bridge.py --check --report report.txt
   ```
   El informe dice qué encuentra: carpeta de Popper, columnas de la base de datos, listas, medios, Web Remote,
   puerto de la placa, `DmdDevice.ini`.
4. En **Popper Setup → Emulators**, para **cada** emulador visible, añade como **primera** línea:
   - en el **Launch Script**:
     ```
     curl -s -m 2 -G --data-urlencode "name=[GAMEFULLNAME]" http://127.0.0.1:58090/game/start >nul 2>&1
     ```
   - en el **Close Script**:
     ```
     curl -s -m 2 http://127.0.0.1:58090/game/end >nul 2>&1
     ```
   `curl` viene con Windows 10 desde la versión 1803. Si el puente no está en marcha, estas líneas no bloquean
   nada: fallan en 2 segundos.
5. Haz que el puente arranque con Windows (desde la carpeta del puente):
   ```
   powershell -ExecutionPolicy Bypass -File ponte.ps1 installa
   ```
   Crea una tarea programada al iniciar sesión en Windows, sin ventanas, con prioridad baja, que se reinicia si
   se cierra; y arranca el puente enseguida.
6. Pruébalo: sin ninguna mesa en marcha la placa muestra el menú. Toca una mesa y luego **Jugar**.

### Comandos de `ponte.ps1`

Los comandos son palabras en italiano. Se lanzan así, desde la carpeta del puente:
`powershell -ExecutionPolicy Bypass -File ponte.ps1 <comando>`

| Comando | Qué hace |
|---|---|
| `installa` (instalar) | Arranque con Windows (tarea programada) y arranque inmediato |
| `rimuovi` (quitar) | Quita el arranque con Windows y detiene el puente |
| `avvia` (iniciar) | Inicia el puente |
| `ferma` (detener) | Detiene el puente (y el dmdext que él haya iniciado): el puerto de la placa queda libre |
| `riavvia` (reiniciar) | Detiene y vuelve a iniciar |
| `stato` (estado) | Encendido/apagado, CPU y memoria, arranque con Windows sí/no, puerto COM de la placa, últimas líneas del registro |
| `controllo` (comprobación) | Ejecuta la comprobación (`--check`) y guarda el informe en `%LOCALAPPDATA%\nucleo_bridge\controllo.txt` |

Los mensajes del script están en italiano ("Ponte ACCESO" = puente encendido, "SPENTO" = apagado, "Scheda" = placa).

- Registro: `%LOCALAPPDATA%\nucleo_bridge\ponte.log` (por encima de 5 MB se mueve a `ponte.log.1`).
- Estado desde el navegador del PC: `http://127.0.0.1:58090/status` indica si el puente tiene el puerto USB y si
  hay una partida en curso.
- Opciones a mano: `python nucleo_bridge.py [--popper CARPETA] [--port COMx] [--web-port 80]`.
  `--popper` hace falta si Popper no está en una carpeta estándar (`C:\vPinball\PinUPSystem`,
  `C:\PinUPSystem`, `D:\vPinball\PinUPSystem`).

### Actualizar el puente

1. `ponte.ps1 ferma`
2. Copia los archivos nuevos en la carpeta del puente (las cachés siguen en `%LOCALAPPDATA%\nucleo_bridge`).
3. `ponte.ps1 avvia`

`installa` solo hace falta la primera vez, o si se quitó la tarea programada.

### Detenerlo o quitarlo

- Por un rato: `ponte.ps1 ferma` (vuelve a arrancar en el próximo inicio de sesión de Windows, o con `avvia`).
- Para siempre: `ponte.ps1 rimuovi`, y luego quita las dos líneas `curl` de los scripts de los emuladores en
  Popper.

---

## 5. El menú de mesas

Sin ninguna mesa en marcha y con el puente encendido, la placa es un menú de Popper. Arriba, las listas y tres
botones; en el centro, la mesa seleccionada; abajo, la barra de wheels.

### Los dos estilos

Se elige en el panel, **Estilo del menú**:

| Estilo | Aspecto |
|---|---|
| **Cabina** (predeterminado) | Como el tema predeterminado de Popper: la barra de wheels entre raíles cromados con un tubo de luz cian, la wheel elegida dentro de una esfera de cristal cromada, **Jugar** en cian encendido. |
| **DMD** | Títulos, datos, listas y botones dibujados con puntos de colores como un ColorDMD, el backglass visto a través de una rejilla de LED, marcos cuadrados. El texto de puntos va en mayúsculas. |

### Las listas

| Lista | Qué contiene |
|---|---|
| **Recientes** | Las mesas jugadas, de la última hacia atrás |
| **Nuevas** | Todas las mesas, primero las añadidas más recientemente a Popper |
| **Más jugadas** | Las mesas jugadas al menos una vez, por número de partidas |
| **Favoritas** | Las favoritas marcadas en Popper (si está vacía: "Aún no hay favoritas: márcalas en Popper.") |
| **Todas** | Todas las mesas visibles, en orden alfabético |

Arriba a la derecha, el recuento (por ejemplo "Visual Pinball X 28 / 72") y tres botones:

- **A-Z** — abre **Ir a** (más abajo);
- **aleatorio** (icono de flechas cruzadas) — **Sorpréndeme**: una mesa al azar;
- **engranaje** — el panel de ajustes del DMD (capítulo 7).

### La barra de wheels

- **Arrastra** la barra: la wheel del centro, más grande e iluminada, es la mesa seleccionada. La barra da la
  vuelta.
- **Toca** una wheel: viene al centro.
- **Lánzala** con un gesto rápido: la barra sigue sola, más lejos cuanto más rápido (como mucho un tercio de la
  lista); un arrastre lento se mueve lo que hayas recorrido.
- **Desliza** a la derecha o a la izquierda en la zona central: una mesa adelante o atrás.
- **Con teclado o mando**: las flechas izquierda/derecha mueven la barra, Intro activa el botón seleccionado
  (primero **Jugar**), Esc / Atrás cierra primero las ventanas abiertas (Ir a, instrucciones, confirmaciones).

Una mesa sin wheel muestra su nombre en lugar de la imagen.

### Ir a (A-Z)

- Arriba, las **categorías** de la lista abierta, en el idioma de la placa (Visual Pinball X, Future Pinball,
  Pinball FX, Matamarcianos, Laberintos, Plataformas, Deportes y carreras, Otros arcade, Juegos de PC), cada una
  con cuántas mesas de **esta** lista contiene; la primera, **Todos los juegos**, quita el filtro. Una categoría
  filtra la lista.
- Debajo, las **letras**: una letra salta a la primera mesa que empieza por ella. Las letras por las que no
  empieza ninguna mesa están apagadas; cifras y símbolos son **0-9**.
- Para cerrar: la **✕**, un toque fuera del recuadro, deslizar hacia abajo o Atrás.

### Sorpréndeme

El botón aleatorio muestra una mesa al azar. En su ficha aparece entonces **Otra**, para sacar otra enseguida.

### La ficha de la mesa

La mesa seleccionada llena la pantalla: su backglass detrás (oscurecido), la wheel grande, el nombre,
fabricante y año, partidas y tiempo de juego ("10 partidas, 7 h 16 min de juego"), la última partida (fechas en
formato dd/mm/aaaa), **Favorita** si lo es, las mejores puntuaciones. Los botones:

| Botón | Qué hace |
|---|---|
| **Jugar** | Inicia la mesa en la máquina (a través del Web Remote de Popper) |
| **Instrucciones** | Solo si la mesa tiene tarjeta de instrucciones en Popper: la muestra derecha, a todo lo ancho; desplázate con el dedo, toca (o Atrás) para cerrar. Mientras la página no llega muestra "Cargando..."; también se cierra con la **✕**. Si la página se pierde, la placa la pide dos veces más y luego se cierra con "Las instrucciones no llegaron del PC: inténtalo de nuevo."; si el PC no tiene instrucciones para esa mesa: "No hay instrucciones para esta mesa." |
| **Composición** | Cómo usa la mesa el monitor del PC y esta pantalla (capítulo 8), sin tener que jugarla |
| **Otra** | Tras Sorpréndeme: otra mesa al azar |

Tras **Jugar** aparece "Iniciando..." con el nombre de la mesa; **Atrás** solo oculta esta página. Si tras unos
2 minutos Popper no ha iniciado nada (la placa espera tanto porque el puente reintenta mientras Popper rechaza los inicios, por ejemplo justo después de un cuelgue): "La mesa no se inició: revisa el Web Remote de Popper."

### Escaparate: cuando usas los botones de la máquina en Popper

Cuando alguien usa el joystick/los botones de la máquina en Popper, la placa sigue su selección y muestra el
**escaparate** de esa mesa: backglass, DMD y topper uno tras otro, cada 7 segundos, cada uno en un halo de sus
propios colores, con "Toca para elegir una mesa". Un toque abre el menú en esa mesa.

- Los flyers y las instrucciones no están en el escaparate (en Popper son páginas verticales guardadas de
  lado): las instrucciones se abren con el botón **Instrucciones**.
- Si en Popper está seleccionada una entrada que no es una mesa ("Go Back", "Exit", una lista), la placa
  muestra su acción en grande (por ejemplo "ATRAS", "SALIR", o el nombre de la lista) y debajo "En Popper: …".
- Cuando Popper vuelve a su modo attract, vuelve el menú de la placa.
- Si una mesa se inicia desde Popper (no desde la placa), la placa pasa igualmente al DMD.

### Modo attract

Tras 2 minutos sin tocar la pantalla, la barra gira sola (una mesa cada 8 segundos) y parpadea "Toca para
elegir". No ocurre durante una partida; al terminar una partida los 2 minutos vuelven a empezar, así la mesa
recién jugada se queda en pantalla con las puntuaciones actualizadas.

---

## 6. Durante la partida

### Qué muestra la placa

| Situación | En pantalla |
|---|---|
| Mesa VPX con DMD | El DMD en puntos, con la disposición elegida (capítulo 7) |
| Mesa VPX que aún no ha enviado el DMD | "Cargando..." hasta un minuto, luego la página del juego ("Jugando desde hace 12 min"): es una mesa sin DMD, por ejemplo una alfanumérica (ver capítulo 9) |
| Mesa de Future Pinball con DMD | El DMD, copiado por el puente |
| Pinball FX, MAME, juegos de PC | El escaparate del juego en curso |
| PC en silencio 5 s (sin Popper) | El reloj de puntos, de 12 o 24 horas como el reloj del sistema, si **Reloj cuando el PC calla** está activado |

Mientras el DMD de una partida está en pantalla, los gestos del sistema de NucleoOS (cortina, Inicio, Recientes)
están bloqueados: un roce no puede ocultar la puntuación.

### Gestos y teclas

- **Toque breve** sobre el DMD (o sobre el escaparate de un juego sin DMD, o sobre la página "Jugando" de una mesa que aún no ha enviado el DMD): las **acciones rápidas**.
- **Mantén pulsado 2 segundos** (o Intro / Espacio en un teclado, también sobre el reloj y la página de espera):
  el **panel de ajustes**. Con el panel abierto los gestos del sistema vuelven a funcionar.
- **Atrás** (o Esc) sobre el DMD abre y cierra el panel: nunca cierra la app.

### Acciones rápidas

| Opción | Qué hace |
|---|---|
| **Volver al DMD** | Cierra las acciones rápidas |
| **Mesas** | Recorre las mesas durante la partida; arriba, un botón **Volver al DMD** |
| **Salir del juego** | Cierra la mesa en curso ("Cerrando el juego...") y vuelve el menú |
| **Ajustes del DMD** | Abre el panel de ajustes |
| **Composición de la mesa** | La composición de la mesa en juego (capítulo 8), abierta en la pestaña **Esta pantalla** |

Para cerrar las acciones rápidas sin elegir: toca fuera del recuadro o desliza hacia abajo.

En las mesas VPX la salida va por **Wi-Fi**, directamente al Web Remote de Popper (en ese momento el cable es de
dmdext): la placa debe estar en la misma red que el PC. Si el PC no responde: "El PC no responde por Wi-Fi:
cierra el juego en la máquina." En los demás emuladores la salida va por el cable, a través del puente. Si tras los reintentos el juego no se cierra (también al cambiar de mesa): "El PC no cerró el juego: ciérralo en la máquina."

### Iniciar otra mesa

Desde **Mesas** elige una mesa y toca **Jugar**. La placa pregunta "*X* está en juego. ¿Cerrarlo e iniciar
*Y*?": **Cerrar e iniciar** cierra la mesa en curso e inicia la nueva en cuanto el puente informa del final;
**Cancelar** lo deja todo como está.

### Cuando el cable es del DMD

Durante una partida cuyo DMD pasa por dmdext (las mesas VPX) el cable está ocupado por el DMD: **Sorpréndeme**, las fichas de otras mesas, las **Instrucciones** y la elección de la versión del backglass dicen "Disponible al terminar la partida: durante el juego el cable es del DMD."

### Una mesa que se cierra sola

Si la mesa se cuelga o se cierra a la fuerza (sin el aviso de fin de Popper), la placa lo indica — "*Nombre* se cerró solo: puedes iniciarlo de nuevo desde aquí." — y la deja seleccionada, lista para **Jugar**.

---

## 7. El panel de ajustes

Se abre manteniendo pulsada la pantalla 2 segundos (o con Intro / Espacio / Esc), desde el engranaje del menú o
desde **Ajustes del DMD** en las acciones rápidas. Arriba:

- **la línea de estado**: USB / Wi-Fi, el formato en palabras sencillas ("16 tonos", "16 colores", "RGB",
  "reloj", "Pantalla de prueba", "marcadores"), tamaño, fotogramas por segundo, juego;
- **debajo, la dirección Wi-Fi** que hay que poner en el `DmdDevice.ini` (`ws://…:9090/dmd`; "aún sin
  dirección" si la placa no está en una red); cuando el puente para Popper ha saludado a la placa, en la misma línea también "Puente <versión> · Popper: listo" (o "base de datos no encontrada");
- tres botones: **Volver al DMD** (se lee **Atrás** cuando ningún PC está manejando el DMD) cierra el panel;
  **Pantalla de prueba** (más abajo); **Cerrar el DMD** cierra la app, tras la pregunta descrita a continuación.

Cada cambio se aplica al momento y queda guardado en la placa.

### Cerrar la app

Cualquier forma de salir de DMD Pinball — **Cerrar el DMD**, los gestos Inicio / Recientes, las teclas del
sistema, el botón de cierre de la ventana en el modo escritorio clásico — pregunta antes **"¿Cerrar el DMD?"**: durante una partida "Hay una mesa en juego: la máquina se quedará
sin DMD hasta que se vuelva a abrir la app.", si no "La máquina se quedará sin DMD ni menú de mesas hasta que se
vuelva a abrir la app." **Quedarse** (el botón ya seleccionado) cancela; **Cerrar** sale. Atrás nunca cierra la
app.

### Pantalla

| Ajuste | Qué hace | Predeterminado |
|---|---|---|
| **Disposición para todas las mesas** | Cómo ocupa el DMD la pantalla (tabla abajo). El recuadro muestra el nombre y la descripción de la disposición; si la mesa en juego tiene la suya, lo indica ("Esta mesa tiene la suya: …"). Toca el recuadro o **Elegir con vistas previas** para elegir (capítulo 8). | DMD clásico |
| **Rotación** | 0°, 90°, 180°, 270° en sentido horario, según cómo esté montada la placa | 0° |

Las disposiciones, en el orden en que aparecen las vistas previas:

| Disposición | Qué hace |
|---|---|
| **Automática** | Cada mesa con su mejor disposición según sus imágenes: Arte con ventana si tiene FullDMD, Backglass con hueco si tiene rejilla, si no DMD clásico. Un DMD 128x16 recibe DMD a pantalla completa (filas reconstruidas). |
| **DMD a pantalla completa** | Toda la pantalla sin huecos: las filas se reconstruyen (128x64) y los puntos quedan redondos y grandes. |
| **DMD clásico** | Puntos cuadrados y las proporciones reales, lo más grandes posible; franjas arriba y abajo. |
| **DMD alto** | El DMD a todo el ancho y un poco más alto; franjas arriba y abajo. |
| **DMD estirado** | Toda la pantalla con los puntos originales: la imagen se estira, los puntos quedan pequeños y las filas separadas. |
| **Backglass + DMD** | El backglass de la mesa arriba y el DMD debajo, como en un mueble real. Sin backglass, el DMD queda centrado. |
| **Solo backglass** | Solo el backglass de la mesa, sin DMD: la placa es la pantalla del backglass. |
| **Arte de la mesa** | El arte FullDMD de la mesa arriba y el DMD en una franja ancha debajo. |
| **Arte con ventana** | El arte FullDMD de la mesa en toda la pantalla y el DMD dentro de su ventana. |
| **Backglass con hueco** | El backglass con su rejilla en toda la pantalla y el DMD en su hueco. |

- Las disposiciones con backglass o arte usan las imágenes que el puente envía al empezar la partida: sin
  puente, o para una mesa que no las tiene, no están disponibles.
- **Pantalla girada** (90°, 180°, 270°): las disposiciones con imágenes muestran el DMD clásico; a 90° y 270°
  también DMD alto y DMD a pantalla completa. El panel y la pantalla Composición lo indican: "Pantalla girada:
  las composiciones con imágenes muestran el DMD clásico."
- El reloj ocupa siempre toda la pantalla con puntos redondos, sea cual sea la disposición.

### Menú de mesas

| Ajuste | Valores | Predeterminado |
|---|---|---|
| **Estilo del menú** | **Cabina** / **DMD** (capítulo 5) | Cabina |

### Puntos

| Ajuste | Qué hace | Predeterminado |
|---|---|---|
| **Color** | El color de los DMD monocromos, elegido en una fila de círculos de colores; el rótulo dice el elegido ("Color · Naranja"). El anillo vacío es **Del PC** (el color que envía el PC); luego Naranja, Rojo, Amarillo, Verde, Cian, Blanco. Funciona también por USB: los fotogramas monocromos se recolorean, mientras que los DMD en color (ColorDMD, Serum, paletas) conservan sus colores; **Del PC** mantiene el color de dmdext. | Naranja |
| **Brillo** | Del 10 al 100%, con el dedo o con las teclas; se guarda en ambos casos | 100% |
| **Forma del punto** | **Redondo** o **Cuadrado** | Redondo |
| **Halo** | Un suave halo de luz en los huecos entre los puntos | Desactivado |

### Sistema

| Ajuste | Qué hace | Predeterminado |
|---|---|---|
| **Reloj cuando el PC calla** | Si el PC no envía nada durante 5 segundos (después de haber mostrado algo), la placa muestra la hora en puntos (12 o 24 horas, como el reloj del sistema) con la fecha debajo. Nunca aparece sobre el menú de Popper ni durante una partida. | Activado |
| **Iniciar al encender** | NucleoOS abre DMD Pinball al encender | Desactivado |
| **Puerto de red** | El puerto para el Wi-Fi. Se aplica al confirmar o al cerrar el panel, y se guarda solo cuando la placa escucha de verdad en él. Los errores aparecen bajo el campo ("Puerto: un número de 1 a 65535.", "No se puede abrir el puerto …"): en ese caso sigue el puerto en uso. | 9090 |

### Pantalla de prueba

Durante 10 segundos la placa muestra "TEST" con la cuenta atrás de los segundos, el color elegido en 16
escalones, barras de color (rojo, verde, azul, amarillo, cian, magenta, blanco, gris), un borde y un bloque
lleno en la esquina superior izquierda. Mientras tanto se ignoran los fotogramas del PC. Un toque, Intro,
Espacio o Esc la terminan antes. Sirve para comprobar la orientación, los colores y el brillo de la máquina
**sin PC**: si el bloque no está arriba a la izquierda, cambia la **Rotación**.

---

## 8. La pantalla Composición

Aquí eliges, con vistas previas hechas con las imágenes reales de la mesa, cómo usa la mesa las pantallas de la
máquina. Se abre de tres maneras:

| Desde | Qué elige |
|---|---|
| Panel → **Elegir con vistas previas** | La disposición de **esta pantalla** para **Todas las mesas**: las diez disposiciones en una cuadrícula, todas a la vista. Las vistas previas usan las imágenes de la mesa en juego o de la seleccionada en el menú. |
| Ficha de la mesa → **Composición** | El monitor del PC y esta pantalla para **esa** mesa, sin tener que jugarla |
| Acciones rápidas → **Composición de la mesa** | Lo mismo, para la mesa en juego; se abre en la pestaña **Esta pantalla** |

El monitor del PC se elige mesa por mesa: desde el panel solo se elige esta pantalla. Cierra con la **✕** arriba
a la derecha o con Atrás. Arriba, la línea "Imágenes de esta mesa: …" dice qué tiene la mesa (backglass y su
tamaño, con rejilla, arte FullDMD, backglass doble); mientras llegan: "Las imágenes de la mesa llegan del PC...".

### Pestaña "Esta pantalla"

- **Puntos reales, 1:1**: una lupa que muestra el centro del DMD dibujado al tamaño real de la pantalla, para
  juzgar el tamaño de los puntos y los huecos entre filas antes de salir (una miniatura no puede mostrarlos).
- El nombre de la disposición elegida y qué hace. Para **Automática** en una mesa que no está en juego: "se
  decide al iniciar la mesa".
- **Tamaño de los puntos**: **Ajustados** (a la disposición) u **Originales** (cuadrados, en píxeles enteros,
  nunca estirados).
- **Margen**: 0, 1, 2 o 3 filas y columnas de puntos apagados alrededor del DMD.
- Solo durante una partida con un DMD 128x16: **DMD 128x16: reconstruido** (llevado a toda la altura, el
  predeterminado) o **DMD 128x16: original**.
- Las disposiciones como tarjetas: para una mesa la primera es **Como todas las mesas** (sigue la elección del
  panel), y después solo las posibles con las imágenes que tiene la mesa.

### Pestaña "Segunda pantalla (PC)"

Decide qué muestra el **monitor del backglass del PC** para esta mesa. El puente escribe la elección donde B2S y
dmdext la leen al iniciar la mesa (el archivo `.res` de la mesa, `B2STableSettings.xml`, la sección del juego en
el `DmdDevice.ini`), después de hacer **una copia de seguridad** de cada archivo con el sufijo `.prima-di-s2`.

| Opción | En el monitor del PC |
|---|---|
| **Como venía** | Como lo coloca el archivo del backglass de la mesa (B2S), sin cambios |
| **Backglass** | Solo el backglass, en todo el monitor |
| **Backglass + DMD** | El backglass arriba y el DMD debajo |
| **Backglass con rejilla** | El backglass entero con su rejilla |
| **Arte FullDMD** | El arte FullDMD de la mesa (la segunda imagen, más pequeña, del archivo B2S) |
| **Backglass + arte** | El backglass arriba y debajo el arte FullDMD |

Solo aparecen las opciones posibles para esa mesa. Además:

- **DMD también en el monitor**: **No** (el DMD queda solo en esta placa) o **Sí** (también el DMD virtual de
  dmdext en el monitor). Con **Como venía** estos botones están desactivados: "Primero elige una de las
  composiciones de abajo."
- **Versión del backglass**: **Auténtico** o **Fantasía**, para los backglass dobles: B2S enciende la otra serie
  de imágenes desde la próxima partida.
- Los cambios valen **desde la próxima partida** ("Segunda pantalla lista para la próxima partida"). Si estás
  jugando, aparece **Reiniciar la mesa ahora** para cerrarla y abrirla enseguida con la nueva composición.
- "No hay segunda pantalla en el PC": el puente no encontró un segundo monitor. "Esta mesa no tiene archivo de
  backglass (B2S)": el monitor muestra el backglass de Popper y no hay nada que elegir. "El PC no respondió: no se pudo describir el archivo del backglass.": el PC no describió el archivo del backglass tras 3 intentos; vuelve a abrir la Composición.

---

## 9. Solución de problemas

| Problema | Qué hacer |
|---|---|
| **No hay DMD en la placa** | Comprueba que la app esté abierta **antes** de iniciar la mesa; que el `DmdDevice.ini` tenga `[zedmd] enabled = true` (USB) o `[networkstream]` con la URL correcta (Wi-Fi); abre el panel y mira la línea de estado (USB / Wi-Fi, fotogramas por segundo). |
| **dmdext dice "ZeDMD is not available"** | La app no estaba abierta cuando arrancó la mesa, u otro programa tiene ocupado el puerto COM. Activa **Iniciar al encender**, cierra los programas que usen el puerto y, si hace falta, fija el puerto con `port = COMx` bajo `[zedmd]`. |
| **La placa se reinicia al arrancar una mesa** | Al abrir el puerto, dmdext envía a la placa una señal de reinicio: la app la bloquea **solo mientras está abierta**. Mantenla abierta (Iniciar al encender). Si aun así se reinicia, quita el hub: un hub sin alimentación deja la placa sin corriente. |
| **No aparece el menú de mesas** | Abre el panel: si en la línea de la dirección falta la parte "Puente …", el puente no está hablando con la placa. El puente debe estar encendido: `ponte.ps1 stato` ("Ponte ACCESO" y "Scheda: COMx"). Si la placa aparece como "non collegata" (no conectada), revisa el cable. Ejecuta `ponte.ps1 controllo` y lee el informe. Durante una partida no hay menú: aparece al terminar. Con el puente en el cable, la placa le pide el saludo sola cada 3 segundos, así que al volver a abrir la app el menú vuelve solo. |
| **"Popper: base de datos no encontrada"** en el panel | El puente no encuentra la base de datos de Popper: revisa la carpeta de Popper con `ponte.ps1 controllo` (si está en una carpeta no estándar, inicia el puente con `--popper`). |
| **"No hay mesas en esta lista."** | Normal en **Recientes** o **Más jugadas** si aún no has jugado; prueba **Todas**. |
| **"El PC no envió la lista de mesas: revisa Popper."** | `ponte.ps1 controllo`: ¿se encontraron la carpeta y la base de datos de Popper? |
| **Faltan imágenes (wheel, backglass)** | La primera vez las imágenes llegan del PC de una en una; con microSD, con el menú en reposo, la placa las descarga sola y después todo es instantáneo. El archivo en Popper debe llamarse como el juego (o "Nombre(1).png", "Nombre (2).png"). Una imagen añadida en Popper aparece en 10 minutos como mucho, o al volver a abrir la app. |
| **"El cable al PC está ocupado: inténtalo de nuevo en un momento."** | El cable estaba transfiriendo una imagen grande: vuelve a intentarlo. |
| **"Las instrucciones no llegaron del PC: inténtalo de nuevo."** | La página se perdió en el cable incluso tras dos peticiones más: vuelve a intentarlo en un momento. |
| **"Disponible al terminar la partida: durante el juego el cable es del DMD."** | Normal durante una mesa VPX: la función vuelve al terminar la partida. |
| **"… se cerró solo: puedes iniciarlo de nuevo desde aquí."** | La mesa se colgó o se cerró a la fuerza: toca **Jugar** para iniciarla de nuevo. |
| **"El PC no respondió: no se pudo describir el archivo del backglass."** | El puente no respondió para la Composición: comprueba que esté encendido (`ponte.ps1 stato`) y vuelve a abrir la Composición. |
| **"No hay instrucciones para esta mesa."** | El PC no tiene una tarjeta de instrucciones que enviar para esa mesa: añádela en Popper (pantalla GameHelp). |
| **La mesa no arranca** ("La mesa no se inició: revisa el Web Remote de Popper." / "El PC no inició la mesa: revisa Popper.") | Popper debe estar en marcha con el Web Remote activado (`useWEB = true ;`, Popper reiniciado, componente COM registrado, puerto 80). Prueba a iniciar la misma mesa desde Popper: si tampoco arranca ahí, el problema está en la mesa o en Popper. |
| **No consigo salir del juego desde la placa** ("El PC no responde por Wi-Fi: cierra el juego en la máquina." / "El PC no cerró el juego: ciérralo en la máquina.") | En las mesas VPX la salida va por Wi-Fi: la placa debe estar en la red de la máquina y llegar al Web Remote de Popper (TCP 80) en el PC. Normalmente es accesible tal como se instala; si no, en el Firewall de Windows Defender permite las conexiones entrantes TCP 80 para PinUP Popper en la red privada. Mientras tanto, sal con los botones de la máquina. |
| **¿Qué URL pongo para el Wi-Fi?** | La que aparece en la página de espera de la app o en la parte superior del panel: `ws://<IP de la placa>:9090/dmd`. Si la IP cambia en cada encendido, reserva una IP fija en el router. "Wi-Fi no conectado": conecta la placa a la red de la máquina o usa el USB. |
| **"Puerto: un número de 1 a 65535." / "No se puede abrir el puerto …"** | El puerto escrito no es válido, o la placa no puede abrirlo: elige otro en **Puerto de red**. Hasta que funcione, sigue en uso el anterior. |
| **Mesas alfanuméricas o de segmentos (años 80) sin DMD en la placa** | dmdext convierte solo los displays de segmentos, pero únicamente si VPinMAME le pasa el display: para esa ROM la clave del registro `HKCU\Software\Freeware\Visual PinMame\<rom>\showpindmd` debe valer 1. Haz una copia del registro antes de cambiarla. |
| **"Este DMD (…) no cabe en la pantalla."** | Por Wi-Fi el DMD supera 512x256: usa un tamaño menor en dmdext, o el USB. |
| **Imagen al revés o colores raros** | **Pantalla de prueba**: el bloque lleno debe estar arriba a la izquierda (si no, **Rotación**); para los DMD monocromos elige el **Color**. |
| **Puntos pequeños o "demasiado negro" entre filas** | Usa **DMD a pantalla completa** o **DMD clásico** en lugar de **DMD estirado**; compruébalo con la lupa **Puntos reales, 1:1**. |
| **No aparece el backglass aunque lo he elegido** | Con la pantalla girada, las disposiciones con imágenes muestran el DMD clásico: vuelve a poner la **Rotación** en 0°. |
| **El panel no se abre durante la partida** | Mantén el dedo quieto en la pantalla 2 segundos completos (un toque breve abre las acciones rápidas), o pulsa Atrás. |
| **Tras un cuelgue de VPX, Popper no inicia nada** | El puente cierra solo el servidor B2S que quedó abierto ("closed b2sbackglassserverexe" en el registro). Si no ocurre, comprueba que el puente esté encendido. |

Para informar de un problema, adjunta la salida de `ponte.ps1 stato` y las últimas líneas de
`%LOCALAPPDATA%\nucleo_bridge\ponte.log`.

---

## 10. Actualizar el firmware con seguridad

Los ajustes de DMD Pinball se guardan en la memoria interna de la placa y sobreviven a las actualizaciones.

- **Por Wi-Fi** (la forma normal): **Ajustes → Actualización del sistema**. No hace falta nada especial para el
  DMD.
- **Por el cable USB con esptool**: mientras DMD Pinball está abierta, la placa **ignora el reinicio desde el
  puerto USB** (es lo que la protege de dmdext), así que esptool no podría reiniciarla. Antes de escribir:
  1. cierra la app: vuelve a Inicio y responde **Cerrar** a "¿Cerrar el DMD?", o llama a `GET /api/ui/home`,
     que la cierra sin preguntar;
  2. en el PC, detén el puente, que ocupa el puerto COM: `ponte.ps1 ferma`;
  3. no inicies mesas mientras se escribe;
  4. al terminar: `ponte.ps1 avvia` y vuelve a abrir DMD Pinball (o deja que lo haga **Iniciar al encender**).

---

## 11. Límites conocidos

- Por USB la placa es un DMD 128x32: dmdext adapta los otros tamaños. Por Wi-Fi se muestran tal como llegan,
  hasta 512x256.
- Por Wi-Fi, un solo PC cada vez.
- Con **DMD a pantalla completa** el DMD se actualiza a unos 30 fotogramas por segundo; con las disposiciones de
  puntos cuadrados, unos 52-60.
- De dónde llega el DMD: las mesas VPX lo envían con dmdext por USB; las mesas de Future Pinball que tienen DMD
  las copia el puente con el modo mirror de dmdext; Pinball FX, MAME y los juegos de PC muestran las imágenes
  del escaparate del juego.
- Por el cable, los mensajes grandes (imágenes) a veces llegan dañados: el puente los reenvía hasta que la placa
  los confirma; rara vez puede faltar una imagen hasta la siguiente pasada.
- Sin microSD las imágenes llegan por el cable cada vez.
- Cada lista contiene como máximo 2000 mesas.
- Con la pantalla girada, las disposiciones con imágenes muestran el DMD clásico.
- Los flyers y las instrucciones no aparecen en el escaparate; las mesas ocultas en Popper no aparecen en el menú.
- Los cambios en el monitor del PC valen desde la partida siguiente (o con **Reiniciar la mesa ahora**).
- El Web Remote de Popper solo funciona mientras Popper está en marcha.

---

## 12. Privacidad y red

- La placa habla **solo con el PC de la máquina, en la red local**: recibe el DMD (USB, o Wi-Fi en el puerto
  9090) y, para salir o iniciar una mesa durante una partida VPX, llama al Web Remote de Popper en el PC
  (puerto 80). La app no envía nada a Internet.
- **El servidor Wi-Fi del DMD (puerto 9090) no tiene autenticación** y **el Web Remote de Popper no tiene
  contraseña**: úsalos solo en una red doméstica de confianza, nunca en una red pública y nunca expuestos a
  Internet.
- El puente solo escucha en el propio PC (`127.0.0.1:58090`), lee la base de datos de Popper solo en lectura y
  guarda sus imágenes convertidas en `%LOCALAPPDATA%\nucleo_bridge\cache`. Escribe archivos de B2S y dmdext solo
  cuando eliges una composición del monitor (capítulo 8), con copia de seguridad.
- La placa guarda las imágenes recibidas y las últimas listas en la microSD, en `/sdcard/data/dmd`.
