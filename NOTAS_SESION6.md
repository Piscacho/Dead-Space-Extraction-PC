# Sesión 6 — Audio: interrupción AI, salida WASAPI y mezclador AX

Fecha: 2026-11-04 (20:40-22:30). Sin commits.

## Resultado

- **Suena el vídeo de la intro**: el audio del THP llega a Windows (pico 19.999 de 32.767 durante los
  8,5 s del vídeo; medido con el registro de nivel, sin escuchar).
- **AX funciona a su ritmo real** (~333 tramas de 3 ms por segundo) con el DSP sustituido: el motor de
  sonido del juego ya se ejecuta cada trama.
- **Mezclador AX** listo (ADPCM, PCM8, PCM16, bucles, envolvente de volumen, filtro paso bajo, bus
  principal L/R/S). En lo que se ha probado no hay voces AX activas todavía, así que **no está verificado
  con sonido real del juego**: lo primero que suene por AX lo pondrá a prueba (el registro escribe los
  datos de las 6 primeras voces para revisarlos).
- Arreglado un bloqueo serio de los hilos (ver «Fallos»).

## Piezas nuevas

| Archivo | Qué hace |
|---|---|
| `audio.cpp` / `audio.h` | Salida WASAPI (modo compartido, 32 kHz estéreo, Windows convierte la frecuencia), búfer circular de 0,5 s, informe de nivel cada 5 s |
| `ax.cpp` | Sustituto del DSP para AX: `DSPAssertTask`, buzón (`DSPSendMailToDSP` / `DSPCheckMailToDSP`) y la lista de comandos de AX con el mezclador |
| `threads.cpp` | Interrupción **AI** (número 5): cada bloque de DMA «suena» lo que dura y se pide el siguiente |

### Interrupción AI (audio)
- `AIInitDMA` deja la dirección en `0xCC005030/32` y la longitud (en bloques de 32 bytes) en `0xCC005036`;
  `AIStartDMA` activa el bit `0x8000`. Mientras está activo, la bomba manda cada bloque a WASAPI y lanza la
  interrupción 5. El manejador (`__AIDHandler`) llama al callback registrado sin comprobar nada.
- Formato del bloque: estéreo, s16 big-endian, **derecho primero**, 32 kHz. AX usa bloques de 384 bytes
  (96 muestras = 3 ms) con triple búfer; el reproductor THP registra su propio callback (`fn_80254EE0`),
  mezcla el audio del vídeo en sus búferes y llama después al de AX.

### Ciclo de AX sin DSP
1. Interrupción AI → callback de AX (`fn_803D9AD4`): marca estado 2 y llama a `DSPAssertTask` (`fn_803E197C`).
2. `DSPAssertTask` se sustituye: llama al callback «reanudado» de la tarea (`+0x2C`, `fn_803D9B90`), que con
   estado 2 ejecuta `__AXOutNewFrame` (`fn_803D9874`).
3. `__AXOutNewFrame` construye la lista de comandos y la envía por el buzón: correo `0xBABE0080` y luego la
   dirección. El segundo correo ejecuta la lista (`AxRunCommandList`).
4. `DSPCheckMailToDSP` devuelve siempre «buzón libre» (si no, AX esperaría para siempre).

### Lista de comandos (formato nuevo de AXWii, comprobado con datos reales)
`0x00` SETUP, `0x01` ADD_TO_LR, `0x04` PROCESS (lista de PB), `0x0A` compresor, `0x0D` salida del altavoz
del mando, `0x0B` OUTPUT (volumen, surround, L/R), `0x0E` END. El búfer L/R de OUTPUT es justo uno de los
tres bloques que reproduce la AI.

### PB (bloques de voz)
- Disposición de Dolphin (`AXPBWii`) **sin el campo «updates»**: los desplazamientos desde 116 están
  10 bytes más abajo en memoria. Tabla completa en el comentario de `ax.cpp`.
- Direcciones de muestras: ADPCM en nibbles, PCM8 en bytes, PCM16 en muestras; fin inclusivo; al acabar,
  bucle (con el predictor del bucle) o `running = 0`.
- Referencia: código de Dolphin (`AXWii.cpp`, `AXVoice.h`, `DSPAccelerator.cpp`, `AXStructs.h`).

## Fallos encontrados y arreglados

1. **Cambio de hilo dentro de un manejador de interrupción** (bloqueo total). Con el audio, mucho código
   corre en contexto de interrupción y despierta hilos; el SDK intentaba cambiar de hilo en mitad del
   manejador y la bomba de interrupciones se quedaba marcada como ocupada para siempre. En la consola,
   el despachador desactiva el planificador mientras corren los manejadores (`OSDisableScheduler`).
   Ahora `RunHandler` y la entrega de callbacks de IOS hacen lo mismo (contador en `0x80605520`); el cambio
   de hilo ocurre después, con la expropiación.

2. **Margen de pila de los manejadores** (el vídeo no terminaba). `RunHandler` y `CallGuest` bajaban la pila
   0x800 bytes; los hilos del THP tienen pilas pequeñas y el manejador escribía sobre las estructuras
   `OSThread` vecinas (el hilo lector quedaba corrupto). Con un margen de 0x40, como en la consola, el vídeo
   termina («WiiMovieEnd») y el juego sigue al título.
3. Tras el vídeo, **AX reproduce voces reales**: dos voces PCM16 en bucle, panoramizadas L/R (la música del
   menú, PB `0x805ADF60` / `0x805AE0A0`). El mezclador queda verificado con sonido del juego.

## Pendiente

1. Mezclador AX: buses auxiliares (efectos/reverb), remuestreo polifásico, biquad, compresor.
2. Sincronizar el ritmo de la AI con el reloj de la tarjeta de sonido (ahora va por el reloj del PC).
3. `DSE_NOAUDIO=1` desactiva la salida.
