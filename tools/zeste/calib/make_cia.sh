#!/bin/sh
# Builds zeste-calib.cia / zeste-calib.cxi (178 MB mode, 268 MHz) from build/zeste-calib.elf.
# Needs makerom and bannertool (pass their directory as $1; default: the openmw3ds tools).
set -e
TOOLS=${1:-/c/Projects/openmw3ds/.tools/bin}
cd "$(dirname "$0")"
"$TOOLS/bannertool.exe" makesmdh -s "Zeste calibration" -l "PICA wrap calibration for real hardware" -p "Zeste" -i icon.png -o build/zeste-calib.cia.smdh
"$TOOLS/bannertool.exe" makebanner -i banner.png -a banner.wav -o build/zeste-calib.bnr
for fmt in cia ncch; do
  out=zeste-calib.$([ $fmt = cia ] && echo cia || echo cxi)
  "$TOOLS/makerom.exe" -f $fmt -o $out -target t -exefslogo -elf zeste-calib.elf -rsf zeste-calib.rsf \
    -icon build/zeste-calib.cia.smdh -banner build/zeste-calib.bnr \
    -DAPP_TITLE=ZesteCalib -DAPP_PRODUCT_CODE=CTR-H-ZCAL -DAPP_UNIQUE_ID=0xF2E58 -DAPP_SYSTEM_MODE_EXT=178MB
done
ls -la zeste-calib.cia zeste-calib.cxi
