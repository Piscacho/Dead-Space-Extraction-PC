#!/bin/sh
# Syntax-check every generated ppc/ppc_recomp.*.cpp with clang-cl (8 in parallel)
# and print a summary of the errors. Run from the Recomp folder.
export MSYS_NO_PATHCONV=1
L="/g/Guitar Hero Warrior of rock/Recomp/toolchain/clang+llvm-23.1.4-x86_64-pc-windows-msvc/bin"
VC="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/include"
SDK=$(cygpath -m "$(ls -d "/c/Program Files (x86)/Windows Kits/10/Include/"*/ | tail -1)")
rm -rf logs; mkdir -p logs
ls ppc/ppc_recomp.*.cpp ppc/ppc_func_mapping.cpp | xargs -P 8 -I{} sh -c \
  '"$0/clang-cl.exe" /std:c++20 -fsyntax-only -msse4.1 -Wno-everything -imsvc "$1" -imsvc "$2ucrt" -I ppc -I XenonRecomp/thirdparty/simde /FItools/ppc_indirect.h {} > "logs/$(basename {}).txt" 2>&1' \
  "$L" "$VC" "$SDK"
echo "files with errors: $(grep -l "error" logs/ppc_*.txt | wc -l)"
grep -h "error:" logs/ppc_*.txt | sed 's/.*error: //' | sort | uniq -c | sort -rn | head -20
