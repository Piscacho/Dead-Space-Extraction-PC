# Sesión 10 — Golpe, vibración, cooperativo local, texturas proyectadas, vídeos, git y paquete

Fecha: 2026-11-08. Repositorio git local creado (sin remoto): `git log` para ver los cambios.

## Golpe cuerpo a cuerpo
- Los textos del propio juego (buscados en su RAM con `DSE_MEMSEARCH=melee`) dicen: **agitar el Nunchuk =
  golpe**, agitar el mando = cargar la luz (Glow Worm), girar el mando de lado = fuego alternativo.
- Antes F agitaba el mando (la luz) y por eso el golpe no salía. Ahora: F = agitar Nunchuk (golpe, mando:
  RB), G = agitar mando (luz, mando: LS), R = fuego alternativo. Etiquetas corregidas en el launcher.
- Sin comprobar en el juego: en la prueba automática el nivel aún está en su introducción (no se puede
  atacar). El juego sí detecta el gesto del Nunchuk (`[gesture] Nunchuk swing` en el registro).

## Mandos
- `WPADControlMotor` (0x803EF0A0) → vibración del mando de Xbox de ese jugador. `vibracion = 0` la apaga.
- Cooperativo local: `jugador2 = 1` conecta el canal 1 (Wii Remote + Nunchuk) con el mando
  `mando_jugador2` (0-3, por defecto 1 = el segundo). El jugador 1 sigue con teclado, ratón y
  `mando_jugador1` (por defecto 0; ninguno si coincide con el del jugador 2). Estado de puntero, movimiento
  y botones por jugador. En el launcher: AVANZADO → MANDOS. Sin probar con dos mandos reales.

## Gráficos
- Coordenadas de textura proyectadas: el vertex shader pasa (s, t, q) y el pixel shader divide por q
  (correcto en perspectiva, como GX). Antes se dividía por vértice y se deformaban en superficies grandes.
- 21:9/32:9: un quad 2D a pantalla completa con una textura grande de memoria (vídeos THP, pantalla de
  seguridad) se queda en 16:9 centrado; los fundidos y tintes (texturas pequeñas) y los pasos que leen
  copias de la EFB siguen ocupando todo el ancho.
- Quitadas las herramientas de diagnóstico de un solo uso (WIDELOG, WIDESKIP, VPLOG, TEXWATCH nueva, ZDUMP,
  ZPROBE, DRAWLOG, FOGLOG). Nuevas: `DSE_MEMSEARCH=texto` (+ `DSE_MEMSEARCH_FRAME`), `DSE_SHOT_FROM=n`.

## Paquete
- `tools/empaquetar.ps1` crea `dist/DeadSpaceExtraction/` (DSE.exe, imágenes del launcher, dse.ini sin
  carpeta del juego, LEEME.txt). 31 MB. El registro va a `logs` junto al exe salvo en la estructura de
  desarrollo (`LogFolder()` en config.cpp). Las partidas, en `user\nand` junto al exe.
- Contiene el código del juego recompilado: solo para uso propio, cada uno con su copia del disco.

## Pendiente
- Probar el golpe (F) y el fuego alternativo (R) jugando.
- Juego en línea (cooperativo por red): necesita determinismo (tiempo del juego por fotogramas) y
  sincronización de mandos; es un trabajo propio.
- Interpolación: 10-15 % de matrices sin emparejar en algunas escenas (posibles tirones).
- Efectos de postproceso a resolución nativa; rendimiento con escala 4 en 32:9.
- Reverberación: comprobar de oído.
