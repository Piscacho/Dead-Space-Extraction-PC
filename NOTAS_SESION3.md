# Sesión 3 — AX, hilos, interrupciones, disco y la tubería de vídeo

Fecha: 2026-11-03. Sin commits.

## Resultado

- **El juego entra en su bucle principal y renderiza**: en 20 s envía ~5.000 dibujos, ~31.000 vértices,
  ~1.700 listas de visualización y ~600 copias EFB→XFB (unos 30 fotogramas por segundo).
  El procesador de comandos va **sin un solo comando desconocido**: sigue el flujo con tamaños exactos.
- **Ventana** (`video.cpp`): muestra el XFB (640×448) que el juego entrega a VI. **De momento sale negra**:
  no hay rasterizador, así que las copias EFB→XFB llevan solo el color de borrado (negro en este juego).
- **Lee el disco** desde los archivos extraídos (`/dev/di` + FST en memoria).
- **Mensajes del juego y del SDK en el log** (`[report]`, sustituto de `OSReport` con `printf` básico).
- Sin sonido (AX arranca pero no mezcla) y sin mandos.

## Lo nuevo en el runtime

| Archivo | Qué hace |
|---|---|
| `threads.cpp` | Hilos del juego como **fibras** e **interrupciones** (bomba que lanza VI, alarmas, IOS y PE) |
| `gx.cpp` / `gx.h` | Procesador de comandos GX: FIFO, tamaños de vértice (VCD/VAT), listas, tokens, copias al XFB |
| `video.cpp` | Ventana Win32 que convierte el XFB (YUV 4:2:2) a RGB en cada retrazo |
| `disc.cpp` / `disc.h` | Partición de datos reconstruida desde `juego/DATA` (`sys/` + archivos del FST) |
| `tools/ppc_indirect.h` | Las escrituras a `0xCC008000` (write-gather pipe de GX) van al procesador de comandos |

### Hilos (fibras)
- La Wii tiene un núcleo y el SDK cambia de hilo en `SelectThread` (`OSSaveContext` + `OSLoadContext`).
  Cada hilo del juego es una fibra con su `PPCContext` y su pila; solo corre una a la vez (como en la consola).
- `OSSaveContext(t)` asocia la fibra actual al hilo `t`. `OSLoadContext(t)` desde `SelectThread`
  (`lr 0x803BB340`) cambia a la fibra de `t`, creándola la primera vez con el contexto de `OSCreateThread`.
- Cualquier otro `OSLoadContext` es la vuelta de un manejador de interrupción: salta con `longjmp`
  de vuelta a la bomba (el código que va detrás no debe ejecutarse, p. ej. en `DecrementerExceptionCallback`).
- **Expropiación**: tras lanzar interrupciones, si hay un hilo listo con más prioridad que el actual,
  se llama a `__OSReschedule`, como hace el despachador de la consola.

### Interrupciones (la «bomba»)
- Se ejecuta en `OSRestoreInterrupts`/`OSEnableInterrupts`, en `OSGetTime` (si `MSR[EE]` está activo)
  y, bloqueando hasta que algo quede listo, en el bucle ocioso de `SelectThread` (`lr 0x803BB280`).
- Fuentes: respuestas de IOS; **retrazo de VI a 59,94 Hz** (bit `0x8000` de `0xCC002030` y manejador 24);
  **alarmas** (cola en `0x806054A0`, hora de disparo en `+8`; `DecrementerExceptionCallback`);
  **PE** token (18) y draw done (19), que levanta el flujo de comandos.

### Disco
- El FST se copia en lo alto de MEM1 (`0x817FE400`) y se apunta en `0x80000038` (como hace el apploader).
- `/dev/di`: lectura (`0x71`, longitud y desplazamiento >> 2) desde los archivos; el resto devuelve 1 (éxito).

## Fallos encontrados y arreglados

1. **`mftbu` se traducía como `mftb`** (el más gordo de la sesión). En la 360 un `mftb` devuelve los 64 bits;
   en la Wii, `mftb rD,269` es la mitad alta. `OSGetTime` salía con la mitad alta igual a la baja, y el
   limitador de fotogramas del juego se resincronizaba en cada vuelta sin llegar nunca a dibujar.
   Arreglado en XenonRecomp (`PPC_INST_MFTB`).
2. AX: `__DSP_boot_task` ahora llama al callback «init» de la tarea, que pone a 1 la bandera que espera AX.
3. Entrega de callbacks de IOS: solo los que había al empezar; antes un callback que pedía otra cosa
   creaba un bucle infinito (la pila Bluetooth).
4. Bluetooth: `/dev/usb/oh1/57e/305` deja las peticiones pendientes y `WUDInit` no arranca la pila BTE
   (que esperaba al adaptador en varios bucles). Ningún mando conectado.
5. `__AI_SRC_INIT` (calibración de la interfaz de audio) se omite.

## Sustituciones del SDK nuevas

| Función | Qué se hace |
|---|---|
| `__DSP_boot_task` | Llama al callback init de la tarea (DSP «arrancado» al instante) |
| `__AI_SRC_INIT` | Se omite |
| `WUDInit` | Marca inicializado sin arrancar Bluetooth |
| `VISetNextFrameBuffer` | Avisa a la ventana del XFB a mostrar |
| `OSReport` | Imprime el mensaje en el log |
| `OSSaveContext`, `OSLoadContext` | Fibras |
| `OSEnableInterrupts`, `OSRestoreInterrupts`, `OSGetTime` | Bomba de interrupciones |
| `IOS_*` | Ver sesión 2; ahora con `/dev/di` y Bluetooth |

## Variables de entorno

- `DSE_SHOT=1`: guarda el fotograma en `logs/frame.bmp` cada segundo (`tools/bmp2png.py` lo pasa a PNG).
- `DSE_PEEK=0xA,0xB,...`: el vigilante imprime esas palabras de memoria cada 2 s.
- `DSE_IOSLOG=1`, `DSE_NOWATCHDOG=1`, `DSE_NOWINDOW=1`.

## Pendiente

1. **Rasterizador** (lo siguiente para ver imagen): transformar vértices (XF: matrices, viewport,
   proyección), leer los arrays de vértices indexados (CP `0xA0`/`0xB0`), decodificar texturas
   (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR) y aplicar el TEV, al menos lo básico para pantallas 2D.
   La opción práctica para el juego completo es un backend D3D12 como el de Guitar Hero.
2. **Audio**: mezclador de AX en PC (las voces y sus parámetros están en memoria; la DSP no existe).
3. **Mandos**: sustituir `KPADRead` con ratón como puntero del Wiimote, teclado y mando.
4. `SYSCONF` propio (idioma, 16:9); `/dev/es` y WiiConnect24 con respuestas reales.
5. Resolución del XFB: se asume 640×480 para la ventana; el juego usa 640×448 (sobran líneas negras).
