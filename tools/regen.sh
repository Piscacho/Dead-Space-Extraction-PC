#!/bin/sh
# Regenerates the config, the switch tables and the recompiled C++ in ppc/.
# Run from the Recomp folder after changing XenonRecomp or the decomp project.
set -e
python -I tools/make_config.py
python -I tools/make_switch_tables.py
rm -rf ppc
mkdir ppc
./XenonRecomp/build-msvc/XenonRecomp/XenonRecomp.exe config/dse.toml XenonRecomp/XenonUtils/ppc_context.h > recomp.log 2>&1
echo "unrecognized: $(grep -c "Unrecognized" recomp.log)"
echo "switch without table: $(grep -c "no switch table" recomp.log)"
