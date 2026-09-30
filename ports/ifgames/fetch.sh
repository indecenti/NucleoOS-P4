#!/bin/bash
# fetch.sh — pinned sources for the IF catalog (into ports/_src, not committed): the interpreters
# and every story file. The sha256 of each story is also in games.json (gen.py re-checks it).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../_src"
mkdir -p "$src/ifgames"
cd "$src"

get() {   # url file sha256
    if [ ! -f "$2" ]; then
        echo "fetch $1"
        curl -sSfL -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    echo "$3  $2" | sha256sum -c --quiet -
}

# Frotz 2.55 (GPL-2.0-or-later, David Griffith et al.): z-code interpreter, "dumb" stdio interface.
get https://gitlab.com/DavidGriffith/frotz/-/archive/2.55/frotz-2.55.tar.gz frotz-2.55.tar.gz \
    a8c4c4d79a2aa9b39e0efbbd3e0803f1dc5ef36f75f2c2945e295d190575eb09
# Glulxe + CheapGlk (MIT, Andrew Plotkin) — same pins as ports/fetch.sh.
GLULXE_SHA=56ab8743bab565de307bd892c555d8d8897ed517
CHEAPGLK_SHA=14d8aaf6e4150669762bd4646a5368e75c1eeee6
get "https://github.com/erkyrath/glulxe/archive/$GLULXE_SHA.tar.gz" "glulxe-$GLULXE_SHA.tar.gz" \
    f1dcb430fafd451f68b14e62d55c26cf4fd35c49f09ed2b8a0bf0d3544d59e5d
get "https://github.com/erkyrath/cheapglk/archive/$CHEAPGLK_SHA.tar.gz" "cheapglk-$CHEAPGLK_SHA.tar.gz" \
    dbf925ef2ae208c8c44d857f73107d8fc3e30325b2fd17180a903438557a085a
# miniz 3.1.2 (MIT): tinfl inflates the embedded story (stored raw-deflated) at start — same pin as ports/fetch.sh.
get https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz miniz-3.1.2.tar.gz     98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb
[ -d frotz-2.55 ] || tar xzf frotz-2.55.tar.gz
[ -d miniz-3.1.2 ] || tar xzf miniz-3.1.2.tar.gz
[ -d "glulxe-$GLULXE_SHA" ] || tar xzf "glulxe-$GLULXE_SHA.tar.gz"
[ -d "cheapglk-$CHEAPGLK_SHA" ] || tar xzf "cheapglk-$CHEAPGLK_SHA.tar.gz"

# Story files (licences and sources: GAMES.md).
cd ifgames
get "https://ifarchive.org/if-archive/games/glulx/advent430.ulx" \
    advent430.ulx eed07ae73d60f87039df4ef83cd01abdb24be5d3cc3e513b7a0fe310a34967f6
get "https://ifarchive.org/if-archive/games/zcode/devours.z5" \
    devours.z5 a0b0569c2f57a975f868242b9a1dfe400a75e6aef92d5480eb51c81a3150eb37
get "https://raw.githubusercontent.com/hlabrand/tristam-island/b50ed88630da8e3591501e241ebdee50dce6fe61/tristam-en.z3" \
    tristam-en.z3 d178078af04528be0dbc3bb41743ca44d5436b78f10e8ca99e95fddc0a4c2b0f
get "https://ifarchive.org/if-archive/games/zcode/Tangle.z5" \
    Tangle.z5 dd5b510fb04daaa2fa9a40fc94414eee3719a72e274c77288b41629e32470817
get "https://ifarchive.org/if-archive/games/zcode/SoFar.z8" \
    SoFar.z8 b94c01b794a728806f78a9ab5a2d2ac0fcbe04632206d501848c18c809feba1c
get "https://ifarchive.org/if-archive/games/zcode/weather.z5" \
    weather.z5 9038c8045d4fc53892901315db98ebc2e36df37b5ea468e1723d6cbacec906d2
get "https://ifarchive.org/if-archive/games/zcode/lists.z5" \
    lists.z5 5e9cf5f5077975a461013294da134970ee7afe82a6f0c0121785c1446051cd59
get "https://ifarchive.org/if-archive/games/zcode/shade.z5" \
    shade.z5 9c4111f13619766b9c928632271ed21edaa7e4f1ddd8fdbd64102841b17ebf05
get "https://ifarchive.org/if-archive/games/zcode/Wallpaper.zblorb" \
    Wallpaper.zblorb f369e58ce72a4c15d6ed6d38b0a78ce32282e80fac810e3019601cbc552b4d45
get "https://ifarchive.org/if-archive/games/zcode/Dual.zblorb" \
    Dual.zblorb e7c7f6f050600a631a2b7b3c7e2394db58f29334d5a779a2660949c9ee7e2d47
get "https://ifarchive.org/if-archive/games/zcode/Heliopause.zblorb" \
    Heliopause.zblorb 960da5cbffbe424dcdf37aa5cdb424c8b653de338bade139fa7d9a61c00245bb
get "https://ifarchive.org/if-archive/games/zcode/coldiron.z8" \
    coldiron.z8 5811d4d2e31a782d2d2670dc13c3642eb688eee34651fbdfc6cc7523fde53d19
get "https://ifarchive.org/if-archive/games/glulx/btyt.gblorb" \
    btyt.gblorb 0130d70298baf92afaba7f2684080050ffe1941f2e65329ffb1e4d43db56ca61
get "https://ifarchive.org/if-archive/games/zcode/bluechairs.z5" \
    bluechairs.z5 7b50dacf8d8b15e2a6429d2173a7c6d07630f4394587ddf1ddee11508503c25b
get "https://ifarchive.org/if-archive/games/glulx/Rover.gblorb" \
    Rover.gblorb 3561294ef94653f9fad8b6c487ff288ea166289f6b23dd2610175272e430649f
get "https://ifarchive.org/if-archive/games/zcode/hoosegow.zblorb" \
    hoosegow.zblorb 3c93fef58f6fbe49017c949220754d7c87ce481d38322ef607d61996180557da
get "https://ifarchive.org/if-archive/games/competition2016/Pogoman%20GO/pogomanGo_v15/Pogoman%20GO%21.gblorb" \
    PogomanGO.gblorb 7bed26fbeeed66f338ad6df69c744212e4432f7440b68e535178ae6ce0540579
get "https://ifarchive.org/if-archive/games/zcode/bookvol.z5" \
    bookvol.z5 ec1d545f32842e29c69f6b586be88b2773a0f2d24e36c3e84bb7fef80d1c842a
get "https://ifarchive.org/if-archive/games/zcode/Figaro.zblorb" \
    Figaro.zblorb 41d6e2d08af88ad9805d10d26a461e05f0fadd021a1f58d2b8cd54a8f9d9181a
get "https://ifarchive.org/if-archive/games/springthing/2007/Fate.z8" \
    Fate.z8 c11b4cd1ce9d709bea66a33d135b13e8a8937c2a05aa95c07601d083c912ae4c
get "https://ifarchive.org/if-archive/games/zcode/spirit.z5" \
    spirit.z5 258199e5fb7b494d3c6e60cc4fbe466b08f729e90a06753db49f4af7bfdca2d5
get "https://ifarchive.org/if-archive/games/glulx/ArtOfFugue_NoMusic.gblorb" \
    ArtOfFugue_NoMusic.gblorb cf4c8a0dd7929265bda61644da412b9c1c8695dc49fc78faa7232d31560921d5
get "https://ifarchive.org/if-archive/games/glulx/DoctorM.zip" \
    DoctorM.zip e6f3973edebde7577a00ba54c85c40d40d684a9d3ebb950df9353cf1d9b5fc3d
[ -f DoctorM.gblorb ] || unzip -p DoctorM.zip "Release/DoctorM.gblorb" > DoctorM.gblorb
echo "1ff43174d6ec5dec316e7feb1c0fc6ca1fb4dd2d99e080f6204f3f7a395aa762  DoctorM.gblorb" | sha256sum -c --quiet -
get "https://ifarchive.org/if-archive/games/competition2012/zcode/changes/Changes.z8" \
    Changes.z8 f5a11761e072884c43f1c8437d7756c4fcd4279cdfae3e8ddae51caa7a560162
get "https://ifarchive.org/if-archive/games/zcode/risorg.zblorb" \
    risorg.zblorb c3c0ef267892676703727c489faf9bc98532b6a3b51f098d3580e7ee85f51d42
get "https://ifarchive.org/if-archive/games/zcode/italian/Beyond_txt.zip" \
    Beyond_txt.zip d86015865585910b55281de719ff25c382f9f9c4ecd6b491827e2273354c1081
[ -f aldila.zblorb ] || unzip -p Beyond_txt.zip "aldila.zblorb" > aldila.zblorb
echo "ec19542d2b0ed7683f996acda8891882b4e6b38156b9f1698f7a74ae3948df11  aldila.zblorb" | sha256sum -c --quiet -
get "https://ifarchive.org/if-archive/games/zcode/italian/LaPietraDellaLuna_txt.zip" \
    LaPietraDellaLuna_txt.zip 0ea21ef65188018bdcc0c81cbc06c7496ea5e7e95dd29e26db125770763e9f5e
[ -f luna.zblorb ] || unzip -p LaPietraDellaLuna_txt.zip "luna.zblorb" > luna.zblorb
echo "6590d4d68d364e7576880af30a73f2bf64af4a1a3f060a486dfb29b3740b8e90  luna.zblorb" | sha256sum -c --quiet -
get "https://ifarchive.org/if-archive/games/zcode/italian/VillaMorgana_txt.zip" \
    VillaMorgana_txt.zip 8b7f7b405127733b917e696598596129c84d4dfc674a5cb10e78071ab5897cca
[ -f villa.zblorb ] || unzip -p VillaMorgana_txt.zip "villa.zblorb" > villa.zblorb
echo "c09b18e3343715446c86d19ccc0e06b394f20fe02db1738c394d2f5eba86fbdc  villa.zblorb" | sha256sum -c --quiet -
get "http://www.paololucchesi.it/at/files/LeLandeDiErisvalle.zip" \
    LeLandeDiErisvalle.zip bff19e091a0f79c1a6a8b44d49b617a8b742fcb91d5f237541cb8266731e1b99
[ -f erisvalle_acv.ulx ] || unzip -p LeLandeDiErisvalle.zip "erisvalle_acv.ulx" > erisvalle_acv.ulx
echo "ab99b4077184d8ea60c4f7848518cdb0c7db8f286ad608b3dbd60f1b3660a638  erisvalle_acv.ulx" | sha256sum -c --quiet -
get "https://ifarchive.org/if-archive/games/zcode/italian/stregatto.zip" \
    stregatto.zip e144b1820bd143b2de6b7d99cc9d19d42d9a721fcbe599842f4f2b759a0b33d8
[ -f Stregatto.z5 ] || unzip -p stregatto.zip "Italiano/Stregatto.z5" > Stregatto.z5
echo "c1a55fe0077b3842b4402f2df1e31f376fb04bb800e9147c28b78e3f08665078  Stregatto.z5" | sha256sum -c --quiet -
get "https://ifarchive.org/if-archive/games/zcode/cheshire-cat.z5" \
    cheshire-cat.z5 14a33465003c192da70d371aef25664346508346238127b865d273081548e63f
get "https://ifarchive.org/if-archive/games/zcode/italian/zenfactor_spa.z8" \
    zenfactor_spa.z8 ad9f7c4e3fe20ec3ece90fdd6b213e57d192f91dfa3ebf95e52635f39e9da82c
get "https://ifarchive.org/if-archive/games/glulx/Edge%27s%20Valley%20Bigfoot%20Society.gblorb" \
    bigfoot.gblorb 1591192aa7119a7792d8cc279579fb4c0b7e2a730ad9bff790b07e5ed1dc722c
echo "sources ready in $src"
