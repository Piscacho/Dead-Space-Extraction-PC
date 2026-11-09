# Sesión 4 — Backend D3D12: primera imagen del juego

Fecha: 2026-11-03. Sin commits.

## Resultado

- **Primera imagen real**: la pantalla de seguridad del Wii Zapper, correcta (texturas, texto, colores,
  logotipo de Nintendo). Captura en `primera_imagen_zapper.png`.
- El backend D3D12 dibuja ~250 llamadas por segundo, con 6 texturas en caché en esa pantalla.
- El juego se queda en esa pantalla: casi seguro espera que se pulse un botón (no hay mandos todavía).

## Diseño (pensado para ser correcto antes que rápido)

| Archivo | Qué hace |
|---|---|
| `gx_state.h` | Estado de GX compartido: CP, BP, memoria y registros de XF, registros y konst del TEV |
| `gx.cpp` | Procesador de comandos: ahora guarda XF (cargas directas e indexadas), TLUT y pasa los dibujos al backend |
| `gx_vertex.cpp` | Decodifica vértices (directos o indexados en arrays) y los transforma en la CPU: matriz de posición, proyección y viewport de GX → coordenadas de recorte de D3D |
| `gx_texture.cpp` | Decodifica texturas por baldosas: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2 (con paleta de TMEM) y CMPR |
| `gx_d3d12.cpp` | Dispositivo, swap chain, EFB (640×528 RGBA8 + profundidad 24 bits), ubershader del TEV, caché de PSO y de texturas, presentación |

- **Vértices en la CPU**: más fácil de depurar. El viewport de GX (con su desplazamiento de 342) se pliega
  en la matriz de recorte, así D3D recorta bien lo que queda detrás de la cámara.
- **Ubershader del TEV**: un único pixel shader interpreta las 16 etapas desde un constant buffer
  (órdenes BP `0x28`-`0x2F`, combinadores `0xC0`-`0xDF`, selección konst `0xF6`-`0xFD`, registros
  y konst de `0xE0`-`0xE7`). Incluye la comparación de alfa (`0xF3`). No genera shaders por combinación.
- **PSO por estado**: mezcla (`PE_CMODE0`, `0x41`), profundidad (`ZMODE`, `0x40`) y topología.
- **Texturas**: caché por dirección, tamaño, formato y paleta, con hash del contenido una vez por fotograma.
- **Presentación**: en cada copia EFB→XFB se muestra el rectángulo copiado (escalado a la ventana) y se
  presenta; el XFB de la memoria del juego no se escribe. Un fotograma en vuelo (se espera a la GPU).
- Sin D3D12 (o con `DSE_NOD3D12=1`) se vuelve a la ventana GDI anterior.

## Fallos de esta sesión
- HLSL: las variables globales son constantes salvo que sean `static`; `pass` es palabra reservada.

## Variables de entorno nuevas
- `DSE_NOD3D12=1`: sin backend D3D12.
- `DSE_D3DDEBUG=1`: capa de depuración de D3D12.
- `DSE_SHOT=1`: ahora guarda el EFB (640×528) en `logs/frame.bmp` cada 60 fotogramas.

## Pendiente (vídeo)
1. Iluminación (canales de color con luces), niebla, texturas indirectas, tablas de swap del TEV,
   modos de comparación del TEV, alfa de destino.
2. **Copias EFB → textura** (efectos, reflejos, pantallas de carga con fundidos): ahora se ignoran.
3. Cull mode (`GEN_MODE`), desplazamiento de scissor (`0x59`), line width / point size.
4. Rendimiento: pasar la transformación de vértices a la GPU, varios fotogramas en vuelo,
   no esperar a la GPU en cada copia.
5. Relación de aspecto 16:9 / resoluciones altas (render del EFB a mayor escala).

## Lo siguiente
- **Mandos** (para pasar de esta pantalla): sustituir `KPADRead` con ratón como puntero del Wiimote/Zapper,
  teclado y mando.
- Audio (mezclador de AX).
