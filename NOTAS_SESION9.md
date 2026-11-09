# Sesión 9 — Fallos gráficos: culling, niebla, texturas de profundidad, ultrapanorámico

Fecha: 2026-11-07. Sin commits (Recomp sigue sin git).

## Qué usa el juego (medido con `DSE_GXSTATS=1`, cada 200 000 dibujos)
- ~98 % de los dibujos piden **culling** de caras traseras (GEN_MODE bits 14-15 = 1). El backend no hacía
  culling (siempre `CULL_MODE_NONE`).
- **Niebla**: ~13 000 dibujos, y **todos** son la máscara del desenfoque de profundidad (ver abajo).
- **Texturas Z** (BP 0xF5, no 0xF4 como contaba antes): los mismos dibujos de la niebla.
- Canal de color 1: casi nada. Puntos, líneas, logic op, alfa de destino, indirectas: nada.

## Cambios (`runtime/gx_d3d12.cpp`)
- **Culling**: modo de GEN_MODE en la clave del PSO (bits 28-29); 1 = BACK, 2 = FRONT, 3 = no se dibujan
  triángulos. `DSE_CULL=0` lo desactiva, `DSE_CULL=2` invierte el sentido. Comprobado en menús y nivel: no
  desaparece geometría.
- **Niebla** (BP 0xEE-0xF2, misma fórmula que Dolphin: profundidad de 24 bits → distancia → A/(B - z>>shift)
  - C, tipos lineal/exp/exp2/inversos, color 0xF2). **Desactivada por defecto** (`DSE_FOG=1` la activa):
  ver "Desenfoque de profundidad".
- **Copias de profundidad**: con formato de píxel Z24 (BP 0x43 = 3) la copia EFB → textura lee el búfer de
  profundidad (ahora `R24G8_TYPELESS` con SRV `R24_UNORM_X8`), bytes R = z[23:16], G = z[15:8], B = z[7:0].
  `DSE_NOZCOPY=1` vuelve a la copia de color.
- **Texturas Z** (BP 0xF4 sesgo, 0xF5 formato/operación): segundo pixel shader `ps_main_z` que escribe
  `SV_Depth`, solo en los PSO con textura Z (bit 30 de la clave), para no perder el early-z en el resto.
  `DSE_NOZTEX=1` las desactiva.
- **Ultrapanorámico**: los quads planos frente a la cámara que ya cubren toda la pantalla 16:9 no se
  estrechan (`WideFactor`), y las pasadas 2D con el viewport desplazado (centro y ≠ 240: la cadena de
  desenfoque/brillo que dibuja en una esquina de la EFB) tampoco.

## Desenfoque de profundidad (resuelto, opcional)
- Cadena del juego por fotograma (gameplay): escena → 3 copias de profundidad por bandas (640x160 en y=0 y
  160, 640x128 en y=320, mitad de tamaño) → cada una, un quad 2D con textura Z (reemplazar, U24) y niebla
  blanca dibujado en la franja fuera de pantalla (y ≥ 448) → copias R8 160x40/160x32 → máscara 160x112.
  Igual con la escena reducida (640x448 mitad → 160x80 + 160x32) y una cadena de desenfoque (80x56, 40x28,
  20x14...). Composición final con dos texturas 160x112.
- **El fallo era la caché de copias EFB**: el juego lee como UNA textura 160x112 dos copias puestas una tras
  otra en memoria (160x80 y 160x32 justo después). Se devolvía solo la primera, estirada: todo salía borroso
  y ampliado. Ahora `StackedCopyFor` las apila en una textura (se rehace solo si cambia alguna pieza).
- También: si se lee una dirección con otro tamaño que la última copia (una copia pequeña reutilizó el
  principio de su memoria), se usa la copia anterior de ese tamaño del `pool`.
- Ojo, la escena de la videollamada no sirve para analizar: el panel azul (quad a z = -0.349) tapa todo y la
  profundidad 0.9427 constante es correcta allí.
- Opción `profundidad_campo` (launcher: Gráficos → "Desenfoque de profundidad (original)"), apagada por
  defecto. En 21:9/32:9 el desenfoque es más fuerte en horizontal (la imagen reducida tiene 160 píxeles de
  ancho para toda la pantalla).

## Ultrapanorámico: el rectángulo verdoso (resuelto)
- Era la capa de interfaz: el juego dibuja menús, HUD, mira y tintes de pantalla como planos frente a la
  cámara (matriz sin rotación, z = -750, o centrados a z = -85). El tinte verde son un centro y dos laterales
  que llegan a ±1.5 (fuera del 16:9); al estrecharlos como el HUD acababan en ±0.75.
- Ahora la interfaz (2D y esos planos) recibe factor negativo (`-g_wideK`) y el vertex shader estrecha solo
  los vértices dentro del 16:9; los de fuera se quedan fuera, así los tintes cubren toda la pantalla ancha y
  menús/HUD siguen igual.
- Además: los quads 2D que leen una copia EFB son pasos de postproceso y no se estrechan; tampoco las pasadas
  2D con el viewport desplazado (centro y ≠ 240).

## Herramientas de diagnóstico nuevas (variables de entorno)
- `DSE_GXSTATS=1`: uso de funciones de GX. `DSE_FOGLOG=1`: parámetros de niebla y TEV de esos dibujos.
- `DSE_WIDELOG=1`: dibujos 2D, quads grandes en perspectiva y dibujos en espacio de cámara en modo ancho.
- `DSE_ZDUMP=1`: lee una copia de profundidad cada 150 fotogramas (histograma) y lista las escrituras de
  profundidad raras y los limpiados de la EFB.
- `DSE_ZPROBE=n1,n2,...`: lee el búfer de profundidad antes de esos números de dibujo (fotogramas múltiplos
  de 150) y registra 9 puntos.
- `DSE_DRAWLOG=n`: detalle de los dibujos n..n+12 (matrices, viewport, TEV, alfa, colores, bytes de la
  textura 0).
- `DSE_COPYPROBE=fotograma`: guarda la EFB antes de cada copia a textura de ese fotograma
  (`logs/copyprobe_NN.bmp`) y la imagen final, y registra los dibujos entre la última copia y el final
  (factor de ancho, extensión, matriz).
- `DSE_QUADLOG=fotograma`: los quads 2D del primer fotograma con copia de profundidad desde ese número.
- `DSE_VPLOG=1` (viewports distintos), `DSE_TEXWATCH=dirección` (dibujos que leen esa textura),
  `DSE_WIDESKIP=1/2` (no dibuja ciertos dibujos estrechados; pruebas).
- Cuidado: un `getenv` por dibujo bajaba el juego de 30 a 20 fps; todas las consultas son `static`.

## Rendimiento
- 32:9, escala 3, fps 240: 30 fps de juego, 160-180 mostrados, GPU ~4.5 ms por pasada (ventana oculta).

## Launcher con arte de SteamGridDB
- Imágenes en `project/build/launcher/` (créditos en `CREDITOS.txt`): héroes (arte de Natalie y Gabe, Aegis VII
  con el Marcador, interior de la Ishimura, el Marcador), logo oficial con brillo azul (PNG con transparencia)
  y la caja de Wii. API pública usada: POST `https://www.steamgriddb.com/api/public/search/assets` con
  `{asset_type, game_id:[5249795], page, filters:{}, order}`.
- `launcher.cpp`: carga con WIC (`LoadPictures`), escala con filtro Fant una vez por tamaño (`ScaledPicture`,
  caché) y dibuja con `AlphaBlend` (`DrawPicture`, `DrawPictureCover`, `DrawPictureWidth`); `FillAlpha` para
  paneles translúcidos (recortados a esquinas cortadas). Fondo distinto por página, logo y caja en la barra
  lateral, banner con el arte y el logo en Inicio, franja de arte en Gráficos/Audio/Avanzado. Si faltan las
  imágenes, vuelve al diseño anterior (`portada.bmp` y texto).

## Sin consola
- `DSE.exe` se enlaza como aplicación de ventana (`/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup`, project/CMakeLists.txt).
  Si nadie redirige stderr (tools/run.ps1 sí lo hace), `main` lo abre en `logs/juego.log`.
- Acceso directo `Recomp/Dead Space Extraction.lnk` (icono del juego). `Jugar.bat` ahora solo hace `start` y se cierra.
