# Sesión 8 — Launcher, ajustes, pantalla completa, controles, giro del mando y audio con efectos

Fecha: 2026-11-06/07. Sin commits (Recomp sigue sin git).

## Launcher (estilo Guitar Hero, estética Dead Space)
- `runtime/launcher.cpp`: ventana nativa dentro de `DSE.exe`, dibujada con GDI (sin controles comunes):
  fondo negro con líneas de escaneo, paneles cian con esquinas cortadas, títulos en mayúsculas espaciadas
  (Bahnschrift), barra lateral con DEAD SPACE / EXTRACTION / PORT PC / autor y versión.
- Páginas: Inicio (portada `project/build/launcher/portada.bmp` = pantalla de título a 3x, ajustes rápidos y
  resumen de controles), Gráficos, Controles (teclado/ratón y mando por acción; clic para cambiar, clic
  derecho borra, Esc cancela, Restablecer; lista con rueda), Audio (volumen, efectos), Avanzado (carpeta del
  juego con selector, mostrar launcher, abrir partidas y registro). Botones GUARDAR y JUGAR (Intro = jugar).
- Se abre antes del juego salvo con `--jugar`, en pruebas automáticas (`DSE_AUTOSCRIPT`/`DSE_AUTOPRESS`) o con
  `mostrar_launcher = 0`; `--launcher` o Mayús al abrir lo fuerzan. `tools/run.ps1` pasa `--jugar`.
  `DSE_LAUNCHER_PAGE=n` lo abre en una página (capturas).
- Icono: emblema de la Ishimura (`runtime/dse.ico`, recurso en `runtime/dse.rc`; CMake con lenguaje RC).

## Ajustes: `project/build/dse.ini` (`runtime/config.h/.cpp`)
- `clave = valor`; una variable `DSE_<CLAVE>` la sustituye (pruebas). `IniFile` conserva comentarios y
  claves desconocidas al guardar.
- Claves: `escala` (1-6), `fps` (0 = 30 original, 60-240 interpolados), `pantalla_completa`, `panoramico`,
  `ancho_ventana`/`alto_ventana`, `volumen` (0-100), `efectos_audio`, `sens_mando`, `carpeta_juego`,
  `mostrar_launcher`, `tecla_<acción>` y `mando_<acción>`.
- `carpeta_juego` sustituye la ruta fija de `main.dol`. `Jugar.bat` ya no fija variables (tendrían
  prioridad sobre el launcher).

## Ventana y pantalla completa (`video.cpp`, `gx_d3d12.cpp`)
- F11 o Alt+Intro: pantalla completa sin bordes en el monitor de la ventana (comprobado: 5120x1440). La ventana
  se puede redimensionar; el presentador ajusta la cadena de intercambio (`ResizeBuffers`).
- La imagen final (`g_output`) se genera a 480·escala de alto con la relación de aspecto del juego y el
  presentador la dibuja centrada con bandas negras (antes era una copia del tamaño de la ventana).
- **Panorámico 16:9**: `SCGetAspectRatio` (`fn_803E64DC`, SYSCONF IPL.AR) devuelve 1; el juego dibuja y
  adapta los menús a 16:9 (comprobado en el menú de perfiles a 2560x1440).
- El puntero del ratón se calcula sobre la imagen, no sobre la ventana con bandas.
- Fallo corregido: los casos nuevos de `WndProc` usaban `break` y la función no devolvía nada después del
  `switch` (comportamiento indefinido: el juego se cerraba al crear la ventana visible).

## Controles configurables (`runtime/bindings.h/.cpp`, `input.cpp`)
- Tabla de acciones compartida con el launcher: A, B, Z, C, 1, 2, +, −, cruceta, stick (teclas), agitar mando,
  agitar Nunchuk, girar. Nombres de teclas en español (`ESPACIO`, `INTRO`, `MOUSE1`...), del mando estilo
  Xbox (`A`, `RT`, `DARRIBA`...). El stick izquierdo es siempre el Nunchuk y el derecho el puntero.

## Giro del mando (R)
- Herramienta nueva `DSE_WATCH=0xDIR,bytes`: protege la página de memoria del invitado y registra (paso a paso
  con la bandera de traza) qué funciones leen o escriben ese rango. Así se encontró el detector.
- El juego **no busca el mando quieto de lado**: `fn_8025E360` pasa la aceleración x/y del filtro de cada mando
  a `fn_802A1FB0`, que calcula `atan2(-x, y)` y acumula giro continuo (pasos de 2° a 135°, mismo sentido,
  se reinicia tras 0,1 s quieto) en «vueltas» (`gestor+0x30·mando+0x10·índice+0x108`). R produce un giro
  de 90° en 8 lecturas que cumple esas condiciones.
- **Sin confirmar**: en el tramo alcanzable con el guion nadie lee esas «vueltas»; no sé qué arma lo usa.

## Audio (`ax.cpp`, `audio.cpp`)
- Buses auxiliares A/B/C como Dolphin (AXWii): cada voz envía a ellos con sus volúmenes (mixer_control por bus);
  `MIX_AUXx` y `UPL_AUXx_MIX_LRSC` suben los buses al juego y mezclan de vuelta lo que su código de efectos
  devuelve. El juego usa 2 órdenes de efectos por trama. Niveles normales, sin cortes. No escuchado.
- Remuestreo: Catmull-Rom de 4 puntos para el tipo «polifásico» (no la tabla de la consola), lineal o vecino
  según la voz. Corregido: el bit de rampa activaba un canal aunque estuviera apagado.
- `volumen` y `efectos_audio` del `.ini`.

## Pendiente
- Ver con el juego delante: bandas negras en pantalla completa (monitor 32:9), cambio de tamaño en vivo.
- Confirmar el giro (R) con un arma que lo use; oír la reverberación.
- Poner el proyecto en git.

## 21:9 y 32:9
- `aspecto = 4:3 | 16:9 | 21:9 | 32:9` en `dse.ini` (selector «Relación de aspecto» en el launcher; el antiguo
  `panoramico=1` sigue valiendo como 16:9). Para cualquier modo panorámico el juego recibe 16:9.
- 21:9 / 32:9 (tipo «Hor+»): factor `k = (16/9) / aspecto`; el sombreador de vértices multiplica la x de
  recorte por `proj1.w`, que `WideFactor()` decide por dibujo:
  - vistas 3D a todo el ancho (viewport de 640): `k` (más campo de visión a los lados);
  - 2D (ortográficas) a todo el ancho: `k` (HUD y menús en el centro a su tamaño 16:9), salvo las que ya
    cubren todo el ancho (fundidos, efectos a pantalla completa: siguen cubriéndolo);
  - viewports más pequeños (renderizado a textura): sin tocar.
- El EFB se hace más ancho en píxeles reales (`g_scaleX = escala · aspecto/(4:3)`: 5120x1584 a 32:9 con 3x),
  así la imagen ancha no pierde nitidez. Tijera y borrados usan `g_scaleX` en x.
- El puntero del ratón se divide por `k` (la pantalla 16:9 del juego es el centro de la imagen).
- Comprobado con capturas: 32:9 en menús (centrados y sin estirar) y en el juego (escena ampliada, la
  videollamada centrada); 21:9 en el fondo del título (planeta ampliado).
- Límite: el fondo de estrellas de los menús es un plano del escenario pensado para 16:9; a los lados queda
  negro liso. Los vídeos (THP) se estiran al ancho completo.
