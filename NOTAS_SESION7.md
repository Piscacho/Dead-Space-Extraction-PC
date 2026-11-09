# Sesión 7 — Del vídeo al menú: título, perfiles y gráficos de GX

Fecha: 2026-11-04 (21:00-22:30). Sin commits.

## Resultado

- Tras la intro el juego llega a la **pantalla de título** («Press A or B to Start», `titulo.png`), la
  pantalla de carga y el **menú de perfiles** («Select Profile», tres «New Profile», BACK / CREATE,
  `menu_perfiles.png`), con la música del menú sonando por AX. Logo y textos se ven correctos.
- El planeta (Aegis VII) se ve con su iluminación: superficie roja y borde de atmósfera azul
  (`menu_perfiles.png`).
- Con `DSE_AUTOPRESS=1` el juego se queda en el menú: la pulsación automática no elige perfil (falta probar
  con el puntero).

## Cambios

| Archivo | Qué |
|---|---|
| `gx_d3d12.cpp` | Copias EFB → textura en la GPU; borrado solo del rectángulo copiado; samplers con modo S/T por separado y espejo; tablas de swap y modo comparación en el TEV; opciones de depuración |
| `gx_vertex.cpp` | Iluminación de los canales de color (luces XF, ambiente, material, atenuación especular/foco); normales; texgen desde normal/color/emboss; post-matriz («dual tex», XF 0x1012) |
| `gx.cpp`, `gx_state.h` | La copia a textura llama al backend |
| `threads.cpp` | El registro de `[vi]` cuenta también las copias a textura |
| `ios_devices.cpp` | `/dev/es`: ID del título (00010000-RZJE) y su carpeta de datos, para que la NAND sepa dónde guardar |
| `input.cpp` | Pruebas automáticas: `DSE_AUTOPRESS=B/AB`, `DSE_AUTOPOINT` |
| `tools/make_switch_tables.py` | Tablas de salto con el registro índice sobrescrito (ver «Fallos») |

### Copias EFB → textura
- BP 0x52 sin el bit 14: formato en los bits 3-6 (`real = tpf/2 + (tpf&1)*8`), bit 9 escala 1/2 (filtro 2x2),
  bit 15 intensidad (luma YUV). Destino en BP 0x4B (<< 5).
- El resultado se queda en una textura de la GPU. En la memoria del juego se escribe un patrón único y se
  guarda su hash: mientras siga ahí, una textura en esa dirección es la copia de la GPU. Si el juego
  sobrescribe la memoria, se vuelve a decodificar desde la RAM.
- Por ahora el juego solo hace una (480x480 → 240x240, formato R8, en la pantalla del Zapper).

### Iluminación
- Control de canal (XF 0x100E/0x1010): bit 0 material del vértice, bit 1 luz, máscara de luces en bits 2-5
  y 11-14, bit 6 ambiente del vértice, bits 7-8 difusa, bits 9-10 atenuación. Luces en XF 0x600 + 16·i.
- Matemática igual que Dolphin (`LightColor`). Solo el canal 0 (color 1 aún no llega al TEV).

## Opciones nuevas de depuración

| Variable | Efecto |
|---|---|
| `DSE_SHOT=seq` | Guarda una captura cada 60 copias al XFB en `logs/shots/frame_NNN.bmp` |
| `DSE_TEXDUMP=1` | Vuelca cada textura decodificada a `logs/tex/DIRECCION_fFORMATO_AxB.bmp` |
| `DSE_TEXWATCH=<hex>` | Registra los dibujos que usan la textura de esa dirección (vértices, texgen, matriz) |
| `DSE_LIGHTLOG=1` | Registra controles de canal y luces |
| `DSE_AUTOPRESS=B` / `AB` | Pulsa B (gatillo del Zapper) o alterna A y B |
| `DSE_MTXLOG=1` | Registra cada valor nuevo de los índices de matrices (CP 0x30/0x40, XF 0x1018/0x1019) |
| `DSE_AUTOPOINT=x,y` | Fija el puntero IR (-1..1, y hacia abajo); con él aparece la mira y el menú responde |
| `DSE_INDLOG=1` | Cuenta los dibujos con texturas indirectas (de momento ninguno) |
| `DSE_D3DDEBUG=1` | Capa de depuración de D3D12; sus mensajes van al registro cuando algo falla |

## Fallos encontrados

1. Pasar de 4 a 22 samplers estáticos (o usar un array de samplers indexado) hacía fallar todas las PSO
   (`E_INVALIDARG`). Con 12 samplers con nombre y el espejo hecho en el shader funciona.
2. El modo «dual tex» se leía del registro equivocado (0x1018 es el índice de matrices).
3. **Tabla de salto con el índice machacado** (fallo del recompilado). En `GXSetTexCoordGen2` el índice se
   escala a otro registro (`rlwinm r9,r3,2`) y luego `lwzx r3,r3,r9` sobrescribe r3 con la dirección de
   destino. `make_switch_tables.py` elegía r3 (el registro de la comprobación de límites), así que el
   `switch` siempre caía en `default`: las matrices de textura de TEX0-6 nunca se guardaban (índice 0 =
   matriz de posición). Ahora, si el registro se sobrescribe antes del `bctr`, se usa el escalado
   (`r = 9`, `shift = 2`). Era la única tabla afectada de las 197. Se encontró con `DSE_MTXLOG=1`.

## Pendiente

1. ~~Crear perfil se queda esperando~~: resuelto, ver «Continuación».
2. Canal de color 1, texturas indirectas, niebla, copias de profundidad (se copian como color).
3. Revisar el resto de tablas de salto por si hay otros patrones raros (186 `bctr` sin tabla son llamadas
   indirectas; comprobar que ninguna es un `switch`).

## Continuación (misma noche): del menú a la partida

**Resultado: se juega el capítulo 1.** Con un guion de entrada el juego recorre título → perfiles → crear
perfil («Create new profile?» → «Enter your name» → «Profile has been updated successfully») → menú
principal → Story Mode → capítulo 1 «Worlds Apart» → selección de arma → dificultad → **nivel jugable**
(`juego_capitulo1.png`, `juego_marker.png`). La partida se guarda y se carga desde la NAND
(`project/build/user/nand/title/00010000/525a4a45/data`: `banner.bin`, `XSaveFile1-3`).

- El «icono de espera» de CREATE era solo el cursor: había que elegir la ranura después.
- **Guardado «corrupto»**: los ficheros NAND no respondían a `GetFileStats` (ioctl 0xB → tamaño y posición),
  así que la partida parecía vacía. Implementado; además, cada lectura/escritura vuelve a posicionar el
  `FILE*` (C exige un `fseek` entre leer y escribir).
- `fn_80006AA0(nivel, formato, ...)` es el `printf` de depuración del motor (vacío en la versión final);
  con `DSE_GAMELOG=1` sale en el registro (cargas de ficheros, guardado, puertas, cámaras...).
- `DSE_AUTOSCRIPT="t,x,y,BTN;..."`: guion de entrada por tiempo (puntero y pulsación A/B/AB/-). El guion
  completo que llega al nivel (con el perfil ya creado):
  `0,0,0,A;95,0,0,-;100,0.22,0.87,A;101,0,0,-;112,-0.41,-0.02,A;113,-0.41,-0.02,-;125,0.28,0.87,A;126,0.28,0.87,-;140,0.28,0.87,A;141,0,0,-;165,-0.01,-0.37,A;166,-0.01,-0.37,-;172,0.28,0.87,A;173,0,0,-`
  (también `DSE_AUTOPRESS_STOP` y `DSE_AUTOPOINT_AFTER`).

### Pendiente nuevo
1. Jugar de verdad con ratón/mando en la ventana (todo lo anterior se probó con guion y capturas).
2. Rendimiento en el nivel (no medido) y defectos gráficos dentro del juego.
3. «Initial Lighting Failed! Defaulting to mid-light» en el registro del motor: revisar.

## Ventana, audio entrecortado y rendimiento

- **Ventana**: el juego siempre crea su ventana; no salía en la barra porque `tools/run.ps1` (las pruebas
  automáticas) lo arranca oculto a propósito. Para jugar: **`Jugar.bat`** (registro en `logs/juego.log`).
- **Audio entrecortado**, dos causas:
  1. La interrupción AI iba al ritmo del reloj del PC y WASAPI al de la tarjeta, sin colchón: cada retraso
     metía silencios muestra a muestra. Ahora la AI se pide cuando en la cola quedan menos de 100 ms
     (manda el reloj de la tarjeta) y, si la cola se vacía, el audio espera a tener 50 ms antes de seguir.
     El informe `[audio]` cuenta los cortes («underruns»).
  2. El juego iba a cámara lenta en el nivel (~10 fps).
- **Rendimiento en el nivel: de ~10 a ~23 fps** (sin el backend gráfico el juego da 30, así que el resto es
  coste nuestro). Cambios:
  - D3D12 usaba la GPU por defecto (puede ser la integrada AMD); ahora elige la de alto rendimiento y la
    registra (`[d3d12] GPU: NVIDIA GeForce RTX 4080 SUPER`).
  - Dos fotogramas en vuelo (asignador, búfer de subida y montón de descriptores por fotograma): la espera a
    la GPU pasó de 7-25 ms a 0.
  - Las copias EFB recreaban su textura cada vez que cambiaba de tamaño (miles por minuto): ahora hay una
    reserva por dirección y tamaño (de 6,4 a 0,2 ms por fotograma). El patrón de memoria se escribe y se
    comprueba solo en 64 puntos.
  - Hash de texturas de 8 en 8 bytes (de 4,3 a 1,5 ms por fotograma).
  - `getenv` en cada dibujo iluminado, cronómetros activos sin `DSE_PROF`, `ldexp` en `psq_l`/`psq_st`
    (ahora multiplicación exacta por potencia de dos, en `ppc_context.h`) y luces calculadas dos veces
    (color y alfa) cuando comparten máscara.
- Herramientas nuevas: `DSE_PROF=1` (tiempos del backend cada 100 fotogramas) y `DSE_SAMPLE=1` (perfilador
  por muestreo: funciones más frecuentes cada 10 s).

### Pendiente de rendimiento
Lo que queda caro (muestreo en el nivel): transformar vértices en la CPU (~20%, unos 170.000 vértices por
fotograma), `GxBackendDraw` (~8%, ~15.000 dibujos por fotograma) y el driver. La solución de fondo es
transformar en la GPU (sombreador de vértices con las matrices XF) y agrupar dibujos.

## Transformación de vértices en la GPU (30 fps en el nivel)

- La CPU ya solo lee los atributos crudos de cada vértice (`GxVertex`: posición, normal, color 0,
  8 coordenadas de textura y las filas de matriz de posición y de textura). Matrices, proyección, viewport,
  iluminación (luces XF) y generación de coordenadas de textura (incluida la post-matriz) las hace
  `vs_main` en `gx_d3d12.cpp`, con las mismas cuentas que tenía la CPU.
- La memoria XF (0x000-0x67F: matrices, matrices normales, post-matrices y luces) va al sombreador como
  `cbuffer XfMem : register(b1)`. Solo se sube de nuevo cuando cambia (`g_xfVersion`, que sube con cada
  escritura en `LoadXf`) o cuando el búfer de subida empieza de cero (`g_uploadEpoch`).
- Los registros XF del dibujo (proyección, viewport, control de canales, ambiente/material, texgens) van en
  el `cbuffer Draw` junto con los del TEV.
- **Error en la primera versión**: el sombreador no compilaba (HLSL no deja indexar con la variable del bucle
  ni las salidas ni las entradas: `o.tc[t]`, `i.tc[source - 5]`) y el juego arrancaba sin D3D12, en el modo de
  respaldo GDI que no dibuja la escena. La primera medición de «30 fps» era de ese modo y las capturas eran
  viejas. Arreglado con copias locales (`tcs`, `itc`).
- **Al verificar**: comprobar `[d3d12] backend ready` en el registro y usar solo capturas con fecha de la
  ejecución (los `frame_NNN.bmp` viejos se quedan en `logs/shots`).
- Resultado medido de verdad (backend activo) en el capítulo 1: **~30 fps** (296 fotogramas cada 10 s), VI a
  tiempo real, 1 corte de audio en todo el recorrido (`juego_escena.png`).

## Resolución interna, mipmaps y movimiento del mando

- **Resolución interna** `DSE_SCALE=1..6` (por defecto **3**: el EFB se dibuja a 1920x1584). Todo lo que ve el
  juego sigue en píxeles de GX (640x528): solo cambian el tamaño del EFB/profundidad, el viewport, la
  tijera y los rectángulos de borrado (multiplicados por la escala). Medido en el capítulo 1: ~29 fps.
- **Copias EFB a tamaño nativo** por defecto (como «Scaled EFB Copy» desactivado en Dolphin): con copias a
  3x, los efectos encadenados (videollamada, desenfoques, que usan la franja y=448-528 del EFB como zona de
  trabajo) salían como ruido en manchas y sin imagen. `DSE_COPYSCALE=1` las vuelve a escalar.
- **Mipmaps + anisótropo 16x**: al decodificar cada textura se genera su cadena de mipmaps (filtro 2x2), pero
  solo se usan si el juego los pide para esa textura (filtro de reducción con mip en TEXMODE0 y LOD máximo
  > 0 en TEXMODE1). Si no, se lee siempre el nivel 0, como el hardware (aplicarlos a todo estropeaba el
  ruido de la videollamada).
- **Movimiento del Wii Remote con teclas**: **F** agita el mando, **G** agita el Nunchuk, **R** (mantenida)
  gira el mando de lado. Agitar = ráfaga de aceleraciones alternas de ±3 g durante al menos 16 lecturas;
  girar = gravedad en x y horizonte a 90°. Se rellenan también `acc_value` y `acc_speed` (mando 0x18/0x1C,
  Nunchuk 0x74/0x78). En `DSE_AUTOSCRIPT` se pueden usar F, G y R. **Sin verificar en el juego**: hace falta
  un enemigo cerca para el golpe y una zona oscura para la barra luminosa.
- `DSE_SHOT_EVERY=n`: captura cada n copias al XFB (a 3x cada captura pesa 12 MB).

## Interpolación de fotogramas (120/240 fps) — en curso

El juego tiene lógica de paso fijo (`fn_80256FE0`: próxima actualización en `0x80604D00`, periodo en
`0x80604CF0`) y partes que cuentan retrazados de VI: quitar la espera (`DSE_NOVSYNC=1`, experimento) dibuja
más pero deja la lógica atascada. Por eso se interpola **a nivel de GX**:

- Cada fotograma del juego (de una copia al XFB a la siguiente) se graba en listas de comandos que se pueden
  volver a ejecutar; las subidas de texturas van a otra lista que se ejecuta una sola vez. La copia al XFB
  dibuja en `g_output` y un **hilo presentador** la copia al back buffer y presenta.
- Cada instantánea de memoria XF guarda la clave del dibujo que la usó primero (`g_gxDrawSource` de `gx.cpp`:
  dirección dentro de la display list o hash de los datos, más el número de aparición en el fotograma). El
  presentador empareja cada instantánea con la del fotograma anterior (91% en el nivel) y vuelve a ejecutar el
  fotograma varias veces con las matrices interpoladas (t = i/k), la última exacta. Si se emparejan menos del
  60%, es un corte de escena y no se interpola.
- `DSE_FPS=120` (o 240) lo activa; sin la variable, un solo pase como antes. 4 ranuras de fotograma; montón de
  descriptores de 262.144 con tablas reutilizadas; estado de dibujo en caché.
- Informe `[present]` cada 5 s: fps mostrados, fps del juego, cortes, % emparejado y tiempo de GPU por pase.

### Estado medido (máquina libre)
- Los `GH6.exe` que usaban ~6 núcleos falseaban las primeras medidas; cerrados con permiso del usuario.
- Dos cambios lo hicieron viable:
  1. **Copia a memoria de la GPU**: al cerrar cada fotograma, su búfer de subida (vértices, constantes) se
     copia una vez a un búfer de VRAM por ranura y los dibujos leen de ahí. Antes cada pase volvía a leer
     ~36 MB de la RAM del PC por PCIe y frenaba al propio juego. Las instantáneas XF siguen en el búfer de
     subida (`Upload(..., live)`), porque el presentador las reescribe entre pases.
  2. **Dibujos agrupados**: dibujos seguidos con el mismo estado y vértices contiguos se emiten como uno, y
     las constantes iguales no se vuelven a subir: de ~14.900 a ~1.200 dibujos por fotograma. GPU por pase:
     de ~10 ms a ~3,5 ms.
- Resultado en el capítulo 1 (3x): `DSE_FPS=120` → ~114 fps mostrados; `DSE_FPS=240` → **~235-240 fps
  mostrados**, juego a 29-30 fps de lógica (velocidad normal), 0-1 cortes de audio, 91% de matrices
  emparejadas, imagen correcta. Sin `DSE_FPS`: 28 fps como antes.
- Los pases por fotograma se calculan con el ritmo nominal (30) y el tiempo de GPU medido, nunca más de los
  que caben; si llega el siguiente fotograma del juego, se salta al pase exacto.
- **`Jugar.bat` usa `DSE_FPS=240` y `DSE_SCALE=3`.**

### Limitaciones
- Un fotograma de retraso (~33 ms).
- No se interpola lo que la CPU anima reescribiendo vértices (partículas) ni las animaciones de textura.
- Los efectos que se realimentan con copias EFB se aplican una vez por pase (pueden verse algo más fuertes).
- Sin verificar con alguien jugando: suavidad real, latencia del puntero y monitores de menos de 240 Hz.

## Parpadeo con la interpolación y controles de movimiento (informe del usuario)

### Parpadeo
- Herramienta: `DSE_RUNSHOTS=n` guarda cada n fotogramas del juego todas las pasadas interpoladas en
  `logs/runs/fNNNNN_rRR.bmp`; `tools/runsheet.py` las junta en una hoja.
- Se veía que pasadas consecutivas del mismo fotograma tenían conos de luz e iluminación distintos. Causas:
  1. **Luces**: el juego reasigna las 8 luces XF a distintos focos cada fotograma; interpolar sus posiciones
     mezclaba luces. Ya no se interpolan (cambian a 30 Hz).
  2. **Objetos que comparten display list** (p. ej. dos conos): se emparejaban por orden de aparición y el
     juego los ordena distinto según el fotograma. Ahora se elige, entre los dibujos del fotograma anterior
     con el mismo origen, el de matrices más parecidas (como máximo 64 candidatos cercanos en el orden).
  3. Además, si una matriz cambia demasiado entre fotogramas (rotación > 0,35 o traslación > 3 + 25%), no se
     interpola (la ranura de matriz era de otro objeto).
- Resultado: los «saltos» descartados bajan de ~5.500 a ~1.500 cada 5 s, y la diferencia entre pasadas es
  continua (la de dos pasadas es ~1,4-2 veces la de una), sin alternancia.

### Movimiento del Wii Remote
- El juego detecta golpes en `fn_8025E5D0`: integra el **cambio** de aceleración en x/y entre muestras
  (descarta muestras donde z cambia 1 g o más) y, pasado un umbral, `fn_8025E1B0` (mando) y `fn_8025E270`
  (Nunchuk) marcan un golpe derecha/izquierda/arriba/abajo en las palabras de gestos (bits 0x02000000 y
  0x04000000 del mando, 0x10000000 y 0x20000000 del Nunchuk). Mi sacudida antigua variaba z ±1,5 g y el
  juego la descartaba siempre.
- Ahora F/G generan una onda cuadrada en x (±2,5 g, cambia cada 2 lecturas, z quieta). Con
  `DSE_GESTURELOG=1` se registran los golpes que reconoce el juego: **F y G verificados** (golpes laterales del
  mando y del Nunchuk).
- R (girar de lado): gravedad pasa a x en rampa y el horizonte gira 90°. **Sin verificar**: con el rifle de
  remaches la retícula no cambia; falta saber qué lee el juego para el disparo alternativo.
- Al arrancar, el juego detecta un golpe «abajo» una sola vez (su filtro parte de 0 g); es antes del título.
