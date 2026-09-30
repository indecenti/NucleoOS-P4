#!/bin/bash
# fetch.sh — pinned inputs of the Game Boy homebrew apps (apps/gbh-*), into ports/_src/gbhomebrew
# (not committed). Every ROM and store screenshot comes from the Homebrew Hub database
# (https://github.com/gbdev/database) at the commit below; each ROM is pinned by sha256.
# Licenses and verification notes: ports/gbhomebrew/GAMES.md.
#
#   bash ports/gbhomebrew/fetch.sh        (Git Bash; uses ports/_src/gbdev-database when it holds
#                                          the pinned commit, otherwise raw.githubusercontent.com)
#
# The emulator core (Peanut-GB + MiniGB APU) is fetched by ports/gameboy/fetch.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dst="$here/../_src/gbhomebrew"
db="$here/../_src/gbdev-database"
GBDB_COMMIT=50293559a496a3e20382fbf6a2e84b70ec622f88
RAW=https://raw.githubusercontent.com/gbdev/database/$GBDB_COMMIT/entries
mkdir -p "$dst/roms" "$dst/db"

# app id | database slug | ROM file in the entry folder | sha256
GAMES='
gbh-5-mazes|5-mazes|5 mazes.gb|4cb6a4a021056164dfad11ba041cb1c36d7453d2ebe6bb7fa9fe4b3eadbc4c60
gbh-5-more-mazes|5-more-mazes|5 more mazes.gb|95d095823ecbf9f31482e7981a06e34a1d5b966d4a5b62bf89f4f89b2f1cf318
gbh-5-mazes-master-levels|5-mazes-master-levels|5 mazes Master levels.gb|abc7cb1a6f83a8770822c189dc2a978116a025cd393106791e1b6c977c8b4034
gbh-airplanz|airplanz|AIRPLANZ.gb|736b821e17f5509d9b0088289977e1232e18338cad923f3e5588300d8622a662
gbh-big2small|big2small|big2small.gb|59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2
gbh-bit-bang|bit_bang|bit_bang.gb|2a4e25370cab3fa7b76a7ca630321b585be70aa4030ac776228c0641154c7735
gbh-carazu|carazu|carazu.gb|eef9dea9ecba15be7b21f3495e8c8bad937a0b2245cac09eec8baf61dfc91fb5
gbh-crossconnect|crossconnect|CrossConnect.gbc|5465e7b4aa37ee0d630d2e63a026f282e8d17c213a2bd9f087417e68061e8b95
gbh-dashy-no-witch|dashy-no-witch|DashyHalloween2019.gb|102e04c6559afec6d007c1ded5c32c14492a1fa680b86384e3fb8100e189bdf1
gbh-game-boy-of-life|game_boy_of_life|life.gb|d491f5d18f0fad55e660277210d1a07a7b52b65c480927e5c78f2f70d824895a
gbh-gb-wordyl|gb-wordyl|GBWORDYL_0.85_en.gb|acc12e3e920e0c432606846da60a5ca9bba21f8012304e8de4a26f88273d14de
gbh-pelmanism|gbccardgame|pelmanism.gb|3dcfc0c06543f3006a2d8344661c9da94b5266c34a4e310d8f42b8f17ded6a0a
gbh-jp|jp|jp.gb|417fa2e7287fb26f27bc90f51e36c3edb7f5a6745654af870b2cb6fa587bdc8a
gbh-postbot|postbot|PostBot.gb|65824d3d7df8003f19923b38055a0660c20542cc363894091da661d9d49d13ad
gbh-rex-run|rex-run-v1-0|rex-run-v1.0.gb|9e074f66648f709528e4fc908dd979d098d74ab54cb24d09a75fa69d674eccaf
gbh-rex-runner|rex-runner-gb|rex-runner.gb|91bd12159d30e86cf4eb0312f28ff1c394e701085d6a8ca641ed92b2bcc8429c
gbh-shock-lobster|shock-lobster|shocklobster.gb|f14efd0340903b1b00d7e8b4b39687d4bc33355ac7742267ccfddfe92f54a611
gbh-squishy-the-turtle|squishy-the-turtle|squishy-magfest.gb|ac1e2eff95cdcf71ed931bd16c88f080038977b8b9dcde433bc023ee6b86c021
gbh-tobu-deluxe|tobutobugirldeluxe|tobudx.gb|0a0e8018dbbc8d7f8cd99f05e7cdc7b4cc9e358ecfe9377ebfb2291a84c6e310
gbh-alien-invasion|alien-invasion|Alien-Invasion.gb|78cbb13080f587c108d8a18661db7d7b4a5c14c730f5bb86794a41132035fbf0
gbh-brekstas-cat|brekstascat|brekstascat_1_3.gb|e46dc09ce51b0bf3ca5c4539350ab7e2d4ea4d428329540605aa0a978ac3ece8
gbh-bustfree|bustfree|bustfree.gb|05640bb1aac6201ab271cf9f6fd9ef5e37fef235514106f81ffa3fcadf06e23a
gbh-slime-trials|canight__slime-trials|SlimeTrials.gb|38879f865221131e83f59e1168417566356fb8139e3e59c76422bd2b05305b14
gbh-toko|demekala__toko|TOKO_jam.gb|dfcda97fdca2c8cdb2ade7c0fbf07d3f32f7cf893c4fd313d5ee1a9df0d7d010
gbh-el-dueloroso|el-dueloroso|ElDueloroso.gb|21b8c78aec985c9c9335db154b186adde9e2aaba32fc7ed76c9cf37cb8ffcf3c
gbh-gb-corp|gb-corp|gbcorp.gb|5a39926a23ff50448859b2d924d8bba5a8c68db91563783f0c593a3aa8e7bad3
gbh-abducted|grimmrobegames__abducted|Abducted.gb|bc8e9612e8cdddc77a5c7ae72f549b95f1356c53eeae929ca02a3bd5278d79cc
gbh-libbet|libbet|libbet.gb|079d161bf2bff4f3baec01339b4f6f02ff6f966c69456885a165b97aac11fa12
gbh-7heaven|nikku4211__7heaven|7HEAVEN_jam.gb|2afb448affe9a4b6de1b34c2b9e6ffd06e5ddbde42387f9a2057eb0cfc5bd38b
gbh-rhythm-land|rhythm-land|rhythm-land.gb|ca7ae6e97011fb423e85514aa9ca245967c01fcfe084ca4232106a39dd6ee9a4
gbh-square-fall|square-fall|square_fall_v01.gb|d16024a37bec3d7efa640bdbdb5ef7e33329302219022f81a59e2963a2c053ed
gbh-skeleton-crew|staticlinkage__skeleton-crew|skeletoncrew_jam.gb|58b34052aa52e7d466432779dadfa2eeed299d6810cf4dd028c01519b444c9cd
gbh-sushi-nights|sushi-nights|SushiNights.gb|5b3203e60e8815acf75e25c5ae369815da047c0f46b81ad3bcc8513ce7167e25
gbh-unstoppable-knight|unstoppable-knight|knight2.gb|42637b68a3d2806062ba410eb848e7ff2c021829a0fb616d15e3b56643f241fc
gbh-wyrmhole|wyrmhole|Wyrmhole.gb|a5e07f89119ee9c93aa50ec1ff828b2441781fa9b1a137f03af7b6c53ce03252
gbh-zypher|zypher|Zypher.gb|1baa54454a61ebb0fef8b34dbd07785d556eb35f381c7b39d7d56f37b034ab81
'

local_db=0
if git -C "$db" cat-file -e "$GBDB_COMMIT^{commit}" 2>/dev/null; then local_db=1; fi

get() {   # entry-relative path -> out file
    [ -f "$2" ] && return 0
    mkdir -p "$(dirname "$2")"
    if [ $local_db = 1 ]; then
        git -C "$db" show "$GBDB_COMMIT:entries/$1" > "$2.part"
    else
        curl -sSfL -o "$2.part" "$RAW/$(python -c 'import sys,urllib.parse;sys.stdout.buffer.write(urllib.parse.quote(sys.argv[1]).encode())' "$1")"
    fi
    mv "$2.part" "$2"
}

echo "$GAMES" | while IFS='|' read -r id slug rom sum; do
    [ -n "$id" ] || continue
    get "$slug/$rom" "$dst/roms/$id.gb"
    echo "$sum  $dst/roms/$id.gb" | sha256sum -c --quiet -
    # the entry metadata + screenshots (store pictures, icon), same commit
    mkdir -p "$dst/db/$id"
    get "$slug/game.json" "$dst/db/$id/game.json"
    python - "$dst/db/$id/game.json" <<'PY' | while read -r shot; do get "$slug/$shot" "$dst/db/$id/$shot"; done
import json, sys
for s in json.load(open(sys.argv[1], encoding="utf-8")).get("screenshots", []):
    sys.stdout.buffer.write((s + chr(10)).encode())   # LF only, also on Windows
PY
done
echo "$GAMES" | awk -F'|' 'NF==4{print $1"|"$2}' > "$dst/games.lst"
echo "gbhomebrew inputs ready in $dst (gbdev/database @ $GBDB_COMMIT)"
