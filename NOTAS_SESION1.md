# Sesión 1 — XenonRecomp adaptado a Wii y primer C++ completo (Dead Space: Extraction, Wii)

Fecha: 2026-11-03. Sin commits.

## Resultado

- **Todo el código del juego se recompila a C++**: 15.119 funciones → `ppc/` (61 archivos, 73 MB).
- **0 errores** al compilar el C++ generado con clang-cl (`tools/check_ppc.sh`, solo sintaxis).
- Quedan **3 instrucciones sin reconocer**, todas dentro de datos que no son código (ver «Pendiente»).
- Aún no hay runtime: el C++ compila pero no se puede ejecutar.

## Punto de partida

- Ejecutable: `dol/main.elf`, copiado de `../decomp/build/RZJE69/main.elf`. Es el ELF que genera el
  proyecto de decompilación, idéntico byte a byte al `main.dol` original y **con secciones**, que es
  lo que el cargador ELF de XenonRecomp necesita.
- Lista de funciones: la de dtk (`../decomp/config/RZJE69/symbols.txt`). La Wii no tiene la tabla
  `.pdata` de la Xbox 360, así que `tools/make_config.py` escribe las 15.051 funciones de dtk, más
  68 puntos de entrada de los ayudantes `_savefpr_N`/`_restgpr_N` de CodeWarrior, en `config/dse.toml`.
- XenonRecomp: copia de la versión local de Guitar Hero (con sus cambios: `bdzf`, cargas con update,
  división segura, reservas, `mftb`...). Compilado con MSVC: `tools/build_msvc.bat`.

## Cambios a XenonRecomp (`xenonrecomp_dse.diff`, sin commit)

1. **`XenonRecomp/gekko.cpp` (nuevo)**: instrucciones de Gekko/Broadway.
   - En Wii el opcode primario 4 son los *paired singles* (en 360 es VMX) y los 56/57/60/61 son
     `psq_l`, `psq_lu`, `psq_st`, `psq_stu`. Se decodifican **antes** que el desensamblador de 360.
   - 38 mnemónicos que usa el juego (~9.200 usos): `ps_add/sub/mul/div/madd/msub/nmadd/nmsub`,
     `ps_muls0/1`, `ps_madds0/1`, `ps_sum0/1`, `ps_merge00/01/10/11`, `ps_mr/neg/abs/nabs`,
     `ps_sel/res/rsqrte`, `ps_cmpu0/1`, `ps_cmpo0/1`, `psq_l(u)(x)`, `psq_st(u)(x)`, `dcbz_l`.
   - Cada FPR tiene dos mitades: `ps0` = `ctx.fN.f64`, `ps1` = `ctx.ps1[N]`.
   - Regla de Dolphin/Gekko: con paired singles activados, **`lfs` y las operaciones de precisión
     simple (`fadds`, `fmuls`, `frsp`...) también escriben `ps1`**. Se añade tras cada una.
   - Cuantización con los GQR (SPR 912-919): tipos float/u8/u16/s8/s16 y escala de 6 bits con signo
     (`ppc_psq_load` / `ppc_psq_store` en `ppc_context.h`).
2. **`RecompileWiiExtra`** (en `gekko.cpp`): instrucciones que la 360 no necesitaba, decodificadas
   desde la palabra cruda cuando el recompilador de 360 no las reconoce:
   - De CodeWarrior: `fcmpo` (5.916), lógica de bits de CR (`crclr`, `cror`, `crset`... ~4.400),
     `stmw`/`lmw`, `rlwnm`, `lswi`/`stswi`, `mcrf`, `mtcrf`, `fctiw`, `lhaux`, `lfdux`, `stfdux`,
     `subfme`, `subfco`.
   - Registros especiales: `mtspr`/`mfspr` genéricos sobre `ctx.spr[1024]` (XER, LR y CTR van a sus campos).
   - Código de supervisor del SDK (excepciones, BATs, SRR0/1, segmentos, caché, TLB): se compila
     como no-op o `return`. No se ejecutará: el runtime sustituirá esas partes del SDK.
   - `tw`/`twi` → `PPC_TRAP`, `sc` → `PPC_SYSCALL` (los implementará el runtime).
   - `rlwimi.`: XenonRecomp no actualizaba `cr0`; ahora sí (6 casos).
3. **`declared_functions_only`** (opción nueva del TOML): solo se recompilan las funciones de la lista.
   Sin ella, XenonRecomp convertía en «funciones» los huecos de `.text`, incluida una **tabla de datos
   con casi todas las instrucciones en orden** en `0x80354890` (probablemente de MetroTRK, el depurador).
4. `.pdata` opcional (las imágenes ELF de Wii no la tienen).

## Hallazgos

| Dato | Valor |
|---|---|
| Base de la imagen | `0x80004000` |
| Código (`.init` + `.text`) | `0x80004000`, 0x434CE4 bytes |
| Instrucciones | ~1,1 millones |
| Paired singles | ~9.200 (sobre todo `psq_l`/`psq_st` en prólogos y epílogos) |
| Llamadas indirectas | 7.244 |

## Comandos

```
tools\build_msvc.bat                                     # compila XenonRecomp
python tools\make_config.py                              # config/dse.toml desde symbols.txt de dtk
XenonRecomp.exe config/dse.toml XenonRecomp/XenonUtils/ppc_context.h   # escribe en ppc/
sh tools/check_ppc.sh                                    # comprueba que el C++ compila
```

## Pendiente / riesgos

- **Sin runtime todavía.** Lo siguiente:
  1. Memoria: reservar el espacio y mapear MEM1 (`0x80000000`, 24 MB) y MEM2 (`0x90000000`, 64 MB),
     con sus espejos sin caché en `0xC0000000`/`0xD0000000` apuntando a la misma memoria.
  2. Arranque: copiar secciones, `.bss` a cero, valores iniciales de r1/r2/r13 y GQR, y llamar a `__start` o `main`.
  3. Sustituir el SDK de Nintendo (OS, DVD, VI, PAD/WPAD, AX, GX) por versiones para Windows.
  4. GX: el juego escribe comandos en la FIFO de `0xCC008000`, a veces desde código inline del juego.
     Hay que interceptar esas escrituras y traducirlas (el núcleo de vídeo de Dolphin sirve de referencia).
- Instrucciones sin reconocer (3): `bdnzfl`, `lswx`, `stswx`, todas en la tabla de datos de
  `0x80354890`. Con `declared_functions_only` ya no se generan; el aviso viene de una función de dtk
  que se solapa con la tabla. Revisar si esa función de dtk está bien delimitada.
- Una llamada sin destino (`// ERROR 803B287C`): dentro de `OSExceptionVector`, que salta a su propio
  interior. Es el vector de excepciones del SDK; en PC no se usa.
- `fcmpo` se trata igual que `fcmpu` (no se modelan las excepciones de coma flotante).
- Las instrucciones de precisión simple redondean a float pero no imitan exactamente el redondeo de
  Gekko en `fmadds` (fusionado en el hardware). Puede causar diferencias mínimas.
- clang-cl: se usa el de `G:\Guitar Hero Warrior of rock\Recomp\toolchain` (no se ha copiado).
