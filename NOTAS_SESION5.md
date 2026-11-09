# Sesión 5 — Mandos: se pasa la pantalla del Zapper y se reproduce el vídeo de la intro

Fecha: 2026-11-03. Sin commits.

## Resultado

- **Mandos** (`runtime/input.cpp`): un Wii Remote con Nunchuk en el canal 0, desde ratón, teclado y
  mando XInput. Con A pulsado, **el juego pasa la pantalla del Wii Zapper**.
- **Se reproduce el vídeo de la intro** (THP): el logotipo de EA distorsionado, al estilo de Dead Space.
  Captura en `video_intro_ea.png`. Sin sonido todavía.
- El reproductor de vídeo crea 3 hilos (lectura, vídeo, audio) que corren bien como fibras.

## Controles

| Wii | Ratón / teclado | Mando (XInput) |
|---|---|---|
| Puntero | ratón sobre la ventana | stick derecho |
| B (gatillo, disparar) | botón izquierdo | gatillo derecho |
| A | botón derecho / espacio | A |
| Z / C (Nunchuk) | Q / E | gatillo izquierdo / LB |
| Stick del Nunchuk | WASD | stick izquierdo |
| + / − | Intro / Tab | Start / Back |
| 1 / 2 | 1 / 2 | X / Y |
| Cruceta | flechas | cruceta |

- El teclado y el ratón solo cuentan con la ventana en primer plano; el mando, siempre.
- `DSE_AUTOPRESS=1` (pruebas): pulsa A unos instantes cada 2 segundos.

## Cómo funciona

- **`KPADRead`** (`fn_803FEF18`, envoltorio de 3 instrucciones) se sustituye entero. Rellena una muestra
  `KPADStatus` de **0xF0 bytes** (este SDK la tiene más grande que la clásica, por MotionPlus).
  Campos confirmados por cómo los lee el juego: botones `+0x0/+0x4/+0x8`, acelerómetro `+0x0C`,
  puntero `+0x20` (−1..1, y hacia abajo), velocidad `+0x28/+0x30`, horizonte `+0x34`, distancia `+0x48`,
  tipo `+0x5C`, error `+0x5D`, puntero válido `+0x5E`, stick del Nunchuk `+0x60`.
- **Conexión**: el juego se entera por callbacks que llamaría la pila Bluetooth. Al registrar
  `KPADSetConnectCallback` (`fn_803FFE98`) se encola `cb(0, 0)` (mando conectado) y al registrar
  `WPADSetExtensionCallback` (`fn_803EECC8`) `cb(0, 1)` (Nunchuk). Se entregan como interrupciones.
- Tipos de dispositivo: **0 mando solo, 1 con Nunchuk, 2 con Classic Controller**.
- Canales 1-3: sin mando (`wpad_err = -1`, 0 muestras).

## Fallos encontrados y arreglados

1. **Los registros especiales eran por hilo**: cada fibra tenía su copia de `ctx.spr`, pero en la Wii
   `HID2` (bit LCE: caché bloqueada activada), los registros DMA, etc. son de la CPU. El juego activaba
   la caché bloqueada en el hilo principal y el decodificador de vídeo, en otro hilo, la veía apagada
   («Fail to prepare»). Ahora se copian al cambiar de hilo, salvo los GQR (que sí son por hilo).
2. **DMA de la caché bloqueada**: escribir `DMA_L` (SPR 923) con el bit de disparo copia entre memoria y
   la caché bloqueada (`0xE0000000`). Ahora XenonRecomp emite `PPC_LC_DMA` y la copia se hace al momento
   (`ppc_lc_dma` en `ppc_context.h`). Lo usa el decodificador THP.

## Pendiente

1. **Audio**: el vídeo lleva sonido (32 kHz estéreo) y el juego usa AX; hace falta el mezclador.
2. Acelerómetro: ahora siempre en reposo. Recargar o algunos gestos (agitar) pueden necesitarlo.
3. Ocultar el cursor del sistema y dibujar un puntero si el juego no lo dibuja.
4. Configuración de controles (un `.ini`).
