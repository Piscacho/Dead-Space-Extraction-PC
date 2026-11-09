# Sesión 2 — Runtime: el juego arranca en PC y llega a su `main()`

Fecha: 2026-11-03. Sin commits.

## Resultado

- **`project/build/DSE.exe` arranca el juego recompilado**: memoria, carga del `main.dol`, `__start`,
  **`OSInit` completo** y **el `main()` del juego** (`0x802573E0`), que empieza a inicializar sus sistemas.
- Durante el arranque el juego ya habla con IOS: configuración (`SYSCONF`), títulos (`/dev/es`),
  WiiConnect24 (`/dev/net/kd`), el disco (`/dev/di`), y crea sus propios archivos en la NAND.
- Se para en la inicialización de **AX** (audio): espera a que el DSP cargue su programa de mezcla.
  Es el siguiente trabajo (ver «Pendiente»).
- Compilación completa: ~1 minuto con `/O1` (`tools\build_game.bat`).

## Runtime (`runtime/`)

| Archivo | Qué hace |
|---|---|
| `main.cpp` | Memoria de invitado de 4 GB; tabla de funciones; llamadas indirectas; ayudantes del recompilador; informe de fallos y vigilante |
| `boot.cpp` | Carga `main.dol` y rellena las variables de memoria baja (`0x80000000`–`0x80003200`) que dejan el arranque y IOS |
| `hooks.cpp` | Sustituciones de funciones del SDK (EXI, DSP, AI, SC) |
| `ios.cpp` / `ios.h` | Sustituto de la capa cliente de IOS (`IOS_Open/Read/Write/Seek/Ioctl/Ioctlv` y sus `Async`) |
| `ios_devices.cpp` | Dispositivos: `/dev/fs` y archivos de NAND (carpeta `user/nand`), STM, y uno genérico que registra lo que se le pide |

- **Memoria**: MEM1 en `0x80000000` y su espejo sin caché en `0xC0000000`; MEM2 en `0x90000000` y
  `0xD0000000`. Son vistas de la misma sección (como en la consola). Los registros de hardware
  (`0xCC000000`, `0xCD000000`) de momento son memoria normal.
- **Tabla de funciones en el lado PC**: la de XenonRecomp iba dentro de la memoria del invitado, justo
  tras la imagen (`0x805FB968`+, ~8,6 MB), es decir, en mitad del heap de MEM1 del juego.
- **Callbacks de IOS como interrupciones**: las respuestas `Async` se encolan y se entregan cuando el
  juego reactiva las interrupciones (`OSRestoreInterrupts`/`OSEnableInterrupts`). Llamarlas dentro de la
  petición rompía el SDK, que marca la petición como pendiente *después* de enviarla.
- **Vigilante**: cada 2 s imprime la pila del hilo principal con nombres (símbolos del `.pdb`).
  Es la herramienta principal para ver dónde se queda esperando. `DSE_NOWATCHDOG=1` lo desactiva.
- Variables: `DSE_IOSLOG=1` registra todas las peticiones a IOS.

## Nombres de funciones del SDK

- `tools/dsy_match.py` compara las funciones con la base de firmas de Dolphin (`Sys/totaldb.dsy`,
  10.549 firmas) usando su mismo hash. **2.970 funciones con nombre** en `config/sdk_names.txt`
  (con librería y objeto, p. ej. `OSAllocFromHeap os.a OSAlloc.o`).
  Se descartan las de 8 bytes: su hash se repite entre funciones distintas.
- dtk ya nombra otras 361 (p. ej. `OSInit`, `main`, `EXISync`, `SCCheckStatus`) en `symbols.txt`.

## Fallos encontrados y arreglados

1. **El `.bss` del DOL incluye `.sdata` y `.sdata2`**: el rango va del primer al último bss. Borrarlo
   tras cargar las secciones dejaba sin datos pequeños al juego (p. ej. la cadena `"/dev/fs"`).
   El juego ya borra sus bss reales en `__init_data`.
2. **Excepciones de coma flotante desenmascaradas**: `ctx.fpscr.csr` guarda una copia de `MXCSR` y la
   escribe entera en cada `mtfsf`. Empezar en 0 las desenmascaraba todas. Ahora empieza con el valor real.
3. **Saltos de `switch` mal traducidos** (el más grave): CodeWarrior escala el índice *en el mismo
   registro* (`rlwinm r0,r0,2,0,29`) antes del `bctr`, y la tabla decía que el índice era `r0`. El
   `switch` comparaba índice×4 con los casos y caía en `__builtin_unreachable()`: el compilador saltaba
   a cualquier sitio. Afectaba a 134 de las 197 tablas. Cambios:
   - XenonRecomp acepta `shift` por tabla (`switch (r >> shift)`) y usa el `default` del TOML.
   - El caso por defecto ya no es comportamiento indefinido: va al `default` o llama a `PPC_TRAP`.
4. **Tablas de salto**: `tools/make_switch_tables.py` las saca del ensamblador de dtk (197 tablas).
   Los otros 186 `bctr` son llamadas virtuales al final de una función y se quedan como llamada indirecta.
5. El vigilante y el informe de fallos atribuían la función equivocada (miraban la tabla de envoltorios,
   que el compilador coloca tras cada cuerpo). Ahora usan los símbolos del `.pdb`.

## Sustituciones del SDK (`hooks.cpp`)

| Función | Motivo |
|---|---|
| `EXISync` | Espera a que el hardware EXI termine la transferencia: se da por terminada (sin dispositivos) |
| `__OSInitAudioSystem` | Arranque del DSP desde `OSInit`: se omite |
| `__AI_SRC_INIT` | Calibra la interfaz de audio esperando su contador: se omite |
| `__DSP_boot_task` | Carga el programa de mezcla de AX en el DSP: se omite (sin sonido) |
| `SCCheckStatus` | Si la lectura de `SYSCONF` sigue pendiente, se marca terminada (el SDK crea una config vacía) |

## Comandos

```
sh tools/regen.sh                   # config + tablas de salto + C++ en ppc/
tools\build_game.bat                # compila project\build\DSE.exe
powershell tools\run.ps1 -Seconds 12   # ejecuta, para y muestra el log con nombres del SDK
```

## Pendiente

1. **AX (audio)**: el juego espera en `sub_803D9BF8` (inicio de AX) la señal del DSP. Hay que sustituir
   AX por un mezclador en PC, o como mínimo dar AX por inicializado para seguir arrancando.
2. **Peticiones de IOS sin respuesta real** (contestadas con 0):
   - `/dev/di` (disco): ioctl `0x12`, `0x86`, `0x95`, `0x96`, `0xE3`. Habrá que leer el disco de verdad
     desde los archivos extraídos (`juego/DATA`) o desde el RVZ.
   - `/dev/es`: ioctlv `0x16`, `0x1B`, `0x1D`, `0x20` (ID del título, vistas de tickets...).
   - `/dev/net/kd/request` ioctl `0x1`, `/dev/net/kd/time` ioctl `0x17` (WiiConnect24).
3. **Vídeo**: VI (modo de vídeo, esperas de retrazo) y GX (la FIFO en `0xCC008000`).
4. **Mandos**: WPAD/KPAD (el Wiimote va por Bluetooth, `/dev/usb/oh1`).
5. `SYSCONF` propio (idioma, 16:9, barra de sensores) en vez de la config vacía.
6. Una llamada indirecta a `0x00000000` desde `__OSThreadInit` (`lr 0x803BAC28`) en las primeras
   pruebas; revisar si sigue pasando.
