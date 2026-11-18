# Prepara una carpeta lista para jugar en otro PC: dist\DeadSpaceExtraction\ con DSE.exe,
# las imágenes del launcher y un dse.ini sin la carpeta del juego (el launcher la pide).
#
# Con -Version 0.9.0 también crea dist\Dead-Space-Extraction-PC-v0.9.0-windows.zip y su .sha256
# para la release. Quien lo use necesita su propia copia del disco extraída (sys\main.dol).
param([string]$Version)
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "project\build"
$out = Join-Path $root "dist\DeadSpaceExtraction"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force (Join-Path $out "launcher") | Out-Null
Copy-Item (Join-Path $build "DSE.exe") $out
Get-ChildItem (Join-Path $build "launcher") -File | Where-Object { $_.Name -ne "portada.bmp" } |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $out "launcher") }
@"
# Dead Space: Extraction (port PC). Lo escribe el launcher; las claves que no conoce se conservan.
escala = 3
fps = 120
aspecto = 16:9
pantalla_completa = 1
mostrar_launcher = 1
"@ | Set-Content -Encoding utf8 (Join-Path $out "dse.ini")
@"
DEAD SPACE: EXTRACTION - PORT PC
================================

1. Necesitas tu propia copia del juego de Wii (RZJE69, versión de EE. UU.) extraída con
   Dolphin: clic derecho en el juego > Propiedades > Sistema de archivos > partición de datos >
   Extraer partición. Esa carpeta (con sys\main.dol dentro) es la "carpeta del juego".
2. Abre DSE.exe. En el launcher: AVANZADO > Carpeta del juego > ELEGIR... y selecciona esa carpeta.
3. Pulsa JUGAR.

Controles: ratón para apuntar, clic izquierdo dispara, clic derecho / espacio = A,
F = golpe (agitar Nunchuk), G = cargar la luz, R = fuego alternativo, WASD = stick del Nunchuk,
Intro = pausa. Todo se cambia en CONTROLES. También funciona con mando de Xbox.

Las partidas se guardan en user\nand y el registro en logs\juego.log, junto a DSE.exe.
Actualizar: el launcher avisa al abrirlo si hay una versión nueva; se instala en AVANZADO >
BUSCAR ACTUALIZACIÓN y conserva dse.ini, las partidas y el registro.
Requisitos: Windows 10/11 de 64 bits y una tarjeta gráfica con DirectX 12.
"@ | Set-Content -Encoding utf8 (Join-Path $out "LEEME.txt")
$size = (Get-ChildItem $out -Recurse | Measure-Object Length -Sum).Sum / 1MB
"Listo: $out ({0:N0} MB)" -f $size
Copy-Item (Join-Path $root "LICENSE"), (Join-Path $root "THIRD-PARTY-NOTICES.txt") $out
if ($Version) {
    $zip = Join-Path $root "dist\Dead-Space-Extraction-PC-v$Version-windows.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $out -DestinationPath $zip
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    "$hash  $(Split-Path -Leaf $zip)" | Set-Content -Encoding ascii "$zip.sha256"
    "Zip: $zip`nSHA-256: $hash"
}
