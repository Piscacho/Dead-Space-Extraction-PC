# Dead Space: Extraction para PC

Recompilación estática para PC de **Dead Space: Extraction** (Wii, RZJE69, EE. UU.) para Windows 10/11.

> **Versión de pruebas (0.x).** Se puede jugar la campaña, pero aún hay partes sin probar a fondo
> (juego en línea entre dos PCs, cooperativo con dos mandos, vibración). Si algo falla, abre un *issue* y adjunta
> `logs\juego.log`.

> **No se incluyen archivos del juego.** Necesitas tu propia copia del disco de Wii de Dead Space: Extraction
> (versión de EE. UU., RZJE69). Dead Space es una marca de Electronic Arts. Este proyecto no está afiliado a EA.

## Descargar y jugar

1. Descarga el `.zip` desde la página de [Releases](https://github.com/Piscacho/Dead-Space-Extraction-PC/releases).
2. Descomprímelo en una carpeta, por ejemplo `C:\Juegos\DeadSpaceExtraction`.
3. Extrae tu disco con Dolphin: clic derecho en el juego → **Propiedades → Sistema de archivos** → partición de
   datos → **Extraer partición**. Esa carpeta (con `sys\main.dol` dentro) es la carpeta del juego.
4. Abre **`DSE.exe`**. Si Windows muestra "Windows protegió tu PC", pulsa **Más información → Ejecutar de todas formas**.
5. En el launcher: **AVANZADO → Carpeta del juego → ELEGIR...**, selecciona esa carpeta y pulsa **JUGAR**.

- **Controles**: ratón para apuntar (Wii Remote), clic izquierdo dispara, clic derecho o espacio = A, WASD = stick
  del Nunchuk, Intro = pausa. **F** = golpe cuerpo a cuerpo (agitar Nunchuk), **G** = cargar la luz (agitar mando),
  **R** = fuego alternativo (girar el mando). También con mando de Xbox, con vibración. Todo se cambia en **CONTROLES**.
- **Gráficos**: resolución interna hasta 6× (3840×2880), hasta 240 fps con fotogramas interpolados (el juego va a 30),
  16:9 y panorámico 21:9 / 32:9 (la interfaz queda centrada), profundidad de campo opcional.
- **Audio**: música y efectos del DSP de la Wii con reverberación.
- **Cooperativo local**: segundo jugador con otro mando (**CONTROLES → Jugador 2**).
- **En línea (experimental)**: cooperativo para 2 sin servidores. Los dos PCs ejecutan el juego y solo se envían
  los controles; los dos necesitáis el mismo `DSE.exe` y vuestra copia del juego. Pestaña **EN LÍNEA**: uno crea la
  partida (abre el puerto 7350 UDP o usa UPnP) y el otro se une con su dirección.
- **Actualizar**: el launcher avisa al abrirlo si hay una versión nueva y la instala desde
  **AVANZADO → BUSCAR ACTUALIZACIÓN**, conservando la configuración, las partidas y el registro.

### Dónde se guarda todo

- Partidas: `user\nand\` junto a `DSE.exe` (en línea, el invitado usa una copia en `user\nand_online\`).
- Configuración: `dse.ini`. Registro de la última partida: `logs\juego.log`.

### Teclas

F11 o Alt+Intro: pantalla completa.

## Compilar desde el código

El repositorio **no contiene nada del juego**: ni el disco, ni `main.dol`, ni el código C++ que XenonRecomp genera a
partir de él (`ppc/`). Cada persona lo genera desde su propia copia.

Requisitos:
- Visual Studio 2022 Build Tools (MSVC 14.4x, con CMake y Ninja) y LLVM (clang-cl, lld-link); la ruta de LLVM se
  configura en `tools\build_game.bat`.
- Python 3.8+.
- `dol\main.elf`: el ejecutable del juego en formato ELF, generado desde tu `sys\main.dol` con
  [decomp-toolkit](https://github.com/encounter/decomp-toolkit) (proyecto dtk para RZJE69).

Pasos (desde la raíz del repositorio, en Git Bash):

```sh
git clone --recursive https://github.com/Piscacho/Dead-Space-Extraction-PC.git
cd Dead-Space-Extraction-PC

# 1. XenonRecomp con el emisor de Gekko (Wii) y los cambios para este juego, compilado en XenonRecomp/build-msvc
cd XenonRecomp && git apply ../xenonrecomp_dse.diff && cd ..

# 2. Generar el código C++ del juego (config/dse.toml ya viene generado)
mkdir ppc
XenonRecomp/build-msvc/XenonRecomp/XenonRecomp.exe config/dse.toml XenonRecomp/XenonUtils/ppc_context.h

# 3. Compilar y crear la carpeta para jugar
cmd //c tools\\build_game.bat
powershell -ExecutionPolicy Bypass -File tools/empaquetar.ps1
```

Modo desarrollo: `project\build\DSE.exe` (las opciones de diagnóstico `DSE_*` están en `NOTAS_SESION*.md`).

### Estructura

| Carpeta | Contenido |
|---|---|
| `runtime/` | Sistema de la Wii (OS, IOS, NAND, disco), GX → Direct3D 12, audio (DSP/AX → WASAPI), entrada, launcher y juego en línea |
| `config/` | Configuración de XenonRecomp (funciones y tablas de saltos del juego) |
| `project/` | CMake y recursos del ejecutable |
| `tools/` | Generación de la configuración, compilación, pruebas y empaquetado |
| `NOTAS_SESION*.md` | Diario técnico de cada sesión de trabajo |

## Requisitos para jugar

- Windows 10 / 11 de 64 bits.
- Tarjeta gráfica con Direct3D 12 y controlador actualizado.

## Créditos

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): recompilador de PowerPC a C++ (adaptado aquí a Gekko).
- [Dolphin](https://github.com/dolphin-emu/dolphin): documentación y referencia del comportamiento de la GPU (GX),
  el DSP y el sistema de la Wii.
- [decomp-toolkit](https://github.com/encounter/decomp-toolkit): análisis del ejecutable del juego.
- Imágenes del launcher: [SteamGridDB](https://www.steamgriddb.com/) (autores en `launcher\CREDITOS.txt`).

## Licencia

MIT ([LICENSE](LICENSE)). Los componentes de terceros mantienen sus propias licencias
([THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)).
