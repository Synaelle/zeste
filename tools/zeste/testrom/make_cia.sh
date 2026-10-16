#!/bin/sh
# Builds zeste-tests.cia / zeste-tests.cxi (178 MB mode, 268 MHz) from build/zeste-tests.elf.
# Needs makerom and bannertool (pass their directory as $1; default: the openmw3ds tools).
set -e
TOOLS=${1:-/c/Projects/openmw3ds/.tools/bin}
cd "$(dirname "$0")"
"$TOOLS/bannertool.exe" makesmdh -s "Zeste tests" -l "Zeste hardware-accuracy test ROM" -p "Zeste" -i icon.png -o build/zeste-tests.cia.smdh
"$TOOLS/bannertool.exe" makebanner -i banner.png -a banner.wav -o build/zeste-tests.bnr
for fmt in cia ncch; do
  out=zeste-tests.$([ $fmt = cia ] && echo cia || echo cxi)
  "$TOOLS/makerom.exe" -f $fmt -o $out -target t -exefslogo -elf zeste-tests.elf -rsf zeste-tests.rsf \
    -icon build/zeste-tests.cia.smdh -banner build/zeste-tests.bnr \
    -DAPP_TITLE=ZesteTests -DAPP_PRODUCT_CODE=CTR-H-ZEST -DAPP_UNIQUE_ID=0xF2E57 -DAPP_SYSTEM_MODE_EXT=178MB
done
ls -la zeste-tests.cia zeste-tests.cxi
