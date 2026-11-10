# Sesión 11 — Juego en línea por lockstep

Fecha: 2026-11-09. Elegido por el usuario: lockstep (los dos PCs ejecutan el juego y solo viajan los
controles), frente a streaming o Parsec.

## Fase 1: determinismo (`threads.cpp`)
- Las interrupciones ya solo se entregan en puntos fijos del código del juego (OSEnableInterrupts,
  OSRestoreInterrupts, OSGetTime). Lo que variaba era el reloj del PC.
- `DSE_DETERMINISTIC=1` / `determinista = 1` (y siempre en línea): `mftb` devuelve una base de tiempo
  virtual (60,75 MHz) que avanza 16 ticks por lectura y 600 por comprobación de interrupciones, y salta al
  siguiente evento (retrazado, bloque de audio, alarma) cuando ningún hilo puede ejecutarse. Retrazado de
  vídeo (59,94 Hz), bloques de audio (AI) y alarmas usan ese reloj. El del PC solo marca el ritmo:
  `PaceToHost` espera en cada retrazado si el PC va adelantado.
- Controles leídos una vez por retrazado (`InputLatchFrame`); los ayudantes de prueba (AUTOSCRIPT...) se
  aplican ahí, antes de enviarse, y usan el reloj del juego.
- **MXCSR**: el controlador gráfico cambiaba el registro de control SSE en el hilo del juego al ejecutar
  comandos GX; con interpolación (otro camino de llamadas D3D12) los cálculos en coma flotante del juego
  daban otro resultado. `Append` (gx.cpp) lo guarda y lo restaura.
- `DSE_HASHLOG=nombre`: hash de cada MB de MEM1/MEM2 cada 60 retrazados en `logs/hash_<nombre>.txt`.
- Comprobado: 4 ejecuciones (escala 3 y 1, sin y con interpolación a 240 fps) con la memoria idéntica en
  los 237 puntos de control (fotogramas 60-14220: vídeos, menús, carga e inicio del nivel).
- `fres`/`frsqrte` se recompilaron como división/raíz IEEE exactas: iguales en Intel y AMD. El juego no lee
  resultados de la GPU en su memoria.

## Fase 2: red (`netplay.cpp`, `netplay.h`)
- Un puerto UDP. El anfitrión (`online = crear`) es el jugador 1 (mando 0); el invitado
  (`online = unirse`, `online_direccion = ip:puerto`) el jugador 2 (mando 1, se conecta solo).
- Antes de arrancar: el invitado saluda con el hash de su DSE.exe; el anfitrión responde con retraso de
  controles, pantalla ancha (el juego debe ver el mismo SCGetAspectRatio) y el tamaño de sus partidas
  guardadas; el invitado las pide en bloques de 1 KB y las pone en `user/nand_online` (sus propias
  partidas no se tocan). Una ventana pequeña muestra el estado (y las direcciones del anfitrión).
- Cada retrazado: los controles de este PC (teclado, ratón, mando 1) se cuantizan (`NetInput`, 12 bytes)
  y se envían para el fotograma +retraso (`online_retraso`, 3 por defecto); cada paquete repite los que el
  otro aún no confirmó (hasta 40). Se espera a los del otro para el fotograma actual (reenvío cada 16 ms;
  20 s sin nada = se sigue solo).
- Cada 120 retrazados se compara un hash de MEM1: si difiere, el registro y la barra de título dicen
  DESINCRONIZADO. La barra de título muestra el estado de la sesión.
- UPnP (adaptado del port de Guitar Hero) abre el puerto del anfitrión en su router 2 horas
  (`online_upnp = 0` lo evita). Sin UPnP: redirigir el puerto o usar Radmin VPN / ZeroTier.
- Launcher: página EN LÍNEA (modo, dirección con campo de texto, puerto, retraso, ayuda).

## Pruebas (dos copias en este PC por 127.0.0.1)
- Anfitrión con el guion de menús, invitado sin controles: 237/237 puntos de control idénticos.
- Invitado enviando controles (puntero, A, B, agitar Nunchuk): 238/238 idénticos, y la partida del
  anfitrión cambia desde el fotograma 60 respecto a la prueba anterior (le llegan los del invitado).
- Fallo encontrado y corregido: los ayudantes de prueba se aplicaban después del intercambio.

## Pendiente
- Prueba real entre dos PCs (otra CPU/GPU, latencia real, NAT).
- El jugador 2 entra en el juego cuando pulsa un botón (cooperativo del propio juego): sin comprobar en
  pantalla en el nivel.
- Desconexión: ahora el que queda sigue solo; podría volver al launcher.
- Pausa del juego en ventanas sin foco, menús del sistema: no hay; si un jugador minimiza, el otro espera.
