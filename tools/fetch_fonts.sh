#!/bin/bash
# The Noto Serif faces tools/ui_assets.py and tools/cia_assets.py draw with (SIL Open Font License 1.1:
# licenses/OFL-1.1.txt), from notofonts/noto-cjk at the tag Serif2.003, checked by sha256, into build/fonts:
# JP for the Latin and Japanese texts, SC for the simplified Chinese ones (the JP subsets lack many of their characters).
# Only needed to change the committed pictures (the build does not use them):
#   tools/fetch_fonts.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/fonts
cd build/fonts
for f in JP/NotoSerifJP-Regular.otf JP/NotoSerifJP-SemiBold.otf JP/NotoSerifJP-Black.otf SC/NotoSerifSC-Regular.otf SC/NotoSerifSC-SemiBold.otf SC/NotoSerifSC-Black.otf; do
  b=${f#*/}
  [ -f "$b" ] || curl -fL --retry 3 -o "$b" "https://github.com/notofonts/noto-cjk/raw/Serif2.003/Serif/SubsetOTF/$f"
done
sha256sum -c - <<'SUMS'
2c9a12dbd4f2408c4610c7ee84a108b62d7236c3775baed618c64d9cb44b2f04  NotoSerifJP-Regular.otf
116d06c2b11ceba33ccb3f8c9eb1c86aba3d5761a1199fd37f74e83365e7a53d  NotoSerifJP-SemiBold.otf
b7197366b775ccb6cd3473b7b09f2c5759a2fdfdbfedf975029203828d0ad386  NotoSerifJP-Black.otf
e8f396decc1f0963a016a989c3d8852e863d1350996f573860a80767c83a1cd3  NotoSerifSC-Regular.otf
517d9736136b3b4e9aea742a7e1acda1922aea91a4425bd8db79281934055bf5  NotoSerifSC-SemiBold.otf
91dde57622845beeb663f300813999febdb7fcd5e451bde199d7c954a9740cb4  NotoSerifSC-Black.otf
SUMS
