#!/bin/bash
# fetch.sh — pinned third-party inputs of the Arduboy apps, into ports/_src/arduboy (not committed).
#
#   bash ports/arduboy/fetch.sh [slug ...]     (Git Bash or WSL; no args = libraries + every game)
#
# Every input is a GitHub source tarball of one exact commit, checked against its sha256:
#   libs/Arduboy2      Arduboy2 library 6.0.0+ (Scott Allen, BSD-3-Clause) — drawing code reused
#   libs/ArduboyTones  ArduboyTones 1.0.3 (Scott Allen, MIT) — only ArduboyTonesPitches.h
#   libs/Arduboy       Arduboy 1.1.1 library (Arduboy LLC, BSD-3-Clause) — for 1.x sketches
#   libs/FixedPoints   FixedPointsArduino (Pharap, Apache-2.0) — header-only, used by some games
#   libs/Tinyfont      Arduboy-TinyFont (Botond Kis, BSD-3-Clause) — 4x4 font used by some games
#   games/<slug>       one repository per game (license, author and notes in GAMES.md)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dst="$here/../_src/arduboy"
mkdir -p "$dst/tar" "$dst/games" "$dst/repos"

want=("$@")
wanted() { [ ${#want[@]} -eq 0 ] && return 0; local w; for w in "${want[@]}"; do [ "$w" = "$1" ] && return 0; done; return 1; }

# repo <dir> <owner/repo> <commit> <sha256>: download + verify + extract (once per commit)
repo() {
    local dir="$1" slug="$2" commit="$3" sum="$4"
    local tgz="$dst/tar/$(echo "$slug" | tr / _)-$commit.tar.gz"
    if [ ! -f "$tgz" ]; then
        echo "fetch $slug@${commit:0:10}"
        curl -sSfL -o "$tgz.part" "https://codeload.github.com/$slug/tar.gz/$commit"
        mv "$tgz.part" "$tgz"
    fi
    echo "$sum  $tgz" | sha256sum -c --quiet -
    if [ ! -f "$dst/$dir/.commit" ] || [ "$(cat "$dst/$dir/.commit")" != "$commit" ]; then
        rm -rf "${dst:?}/$dir"
        mkdir -p "$dst/$dir"
        tar xzf "$tgz" -C "$dst/$dir" --strip-components=1
        echo "$commit" > "$dst/$dir/.commit"
    fi
}
# game <slug> <owner/repo> <commit> <sha256>: one extraction per repository commit (repos/), linked
# as games/<slug> (several games can live in one repository)
game() {
    wanted "$1" || return 0
    local rdir="repos/$(echo "$2" | tr / _)-${3:0:12}"
    repo "$rdir" "$2" "$3" "$4"
    if [ "$(readlink "$dst/games/$1" 2>/dev/null)" != "../$rdir" ]; then
        rm -rf "${dst:?}/games/$1"
        ln -s "../$rdir" "$dst/games/$1"
    fi
}

# ---- libraries ----------------------------------------------------------------------------------
repo Arduboy2 MLXXXp/Arduboy2 bc460a2cff1a3e116880991aa2f88bae4b2e3160 \
    271cd3ad1aa8cc9d1cb468917bf594fadd8c7e0b29edbef1a1ea246ff691bfa7
repo ArduboyTones MLXXXp/ArduboyTones 972fe8117002da47073b6c1835117d654a03e16f \
    a43779d1f435ed6a56971c12aaf323151f6bfa8856d194c8e37f5b3dd03abb6e
repo Arduboy Arduboy/Arduboy 3a8480ce32ff4f7ecfcc7665a868c8ef4658728d \
    09d677b408bb950274302c2240428d68f53046a73e9ec0f3a5adede5f9f7093b
repo FixedPoints Pharap/FixedPointsArduino 149c81247afe207e3e1cd9b47e1885297b7bb887 \
    55f47f7bcfba663556320420deb1837ccb9d6525a57d4d1e1c9e0748357a96b7
repo Tinyfont yinkou/Arduboy-TinyFont c7ab15955ebcbb7f1a714ed8ad545c141137f83b \
    790aad2714db7ce9d075e4ee305338c35ecba61885e1fe07f5a11afb21a1fb63

# ---- games (slug, repository, commit, sha256 of the tarball) --------------------------------------
# GAMES-BEGIN
game crate-confusion phoboslab/arduboy-games de5e8589f20aeb48179b459172e0971f136dbc57 \
    1c39091e2105e6a37f809a26476ef7388a14a555672177311d30b896fb4d9d0a
game hopper obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game samegame obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game reversi obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game lasers obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game beam-em-up unwiredben/arduboy-beamemup 95a7fb6ccd73bc0d8b5803c9cdf02b606b1817dd \
    b2b70a423bf3e58e660ebb568648e0169d04a118377a3fbc7dc11dabf114c1cd
game choplifter ArduboyCollection/Choplifter 6e875e59623b8ff460bdf50d7a543100a01798b2 \
    d12ad5ee06b47dd4ff43fc62df30b1c9f77ec808d6513123c72ff541fd1ed578
game glove ArduboyCollection/glove 81e9988fea3fc68edc984260b17d6f52beb5d497 \
    dddde10fd527c6bb790e4b8369a2e65e78bb21ece42d43dc9ef227d42dd67e2d
game helii ArduboyCollection/Helii-Arduboy f6f9a8f3b3044f0ca8064c9ac16fe41a2985a079 \
    6ada6d84993b061c1966f9541350548f7d557cfc6f291ae99bec5121fc5d1512
game poitto ArduboyCollection/poitto 8e0f8b9581e7de90eac1afb8dd1d5459268e9167 \
    968f7517df0dc2c84d01a55166f3c6b0885649b5e23731b86fe8aba734116f44
game rooftop-rescue BertVeer/Rooftop cb8e9203f62f5ce49423742aa3fb7bc6e1ca3847 \
    1bab4374fe1925b9b8b528f22bb0375031f2dcb10f80b69025a62cbf3314b07e
game to ArduboyCollection/to 7963735f536276affbcf964f80ca3f14d41b392f \
    f2f1c991609b431a625724812fddd4a6ee3dcfbfb1a36b12faf87b284e337aa4
game trench-run lscardinali/TrenchRun-Arduboy 2012f9866dbc97bcd4ccf8e9001a939fb977534b \
    4bc160d97695e13c5d0539f70f8647be28ce6f786cb1398e030d47f4ab945afb
game nineteen43 ArduboyCollection/Nineteen43 bf770656c8b6b2d1a35a9760a39fe2cdd01694d7 \
    6833a7be7a426562011dde645602b3a2513d35316d3dd754b0764638db45520a
game nineteen44 Press-Play-On-Tape/Nineteen44 034bdc57c53da3ffa4ee2e3e5a6494ca726dc82e \
    bfbb76cebd5540dc36b952347464f16e40b5dbb1dc3c93163f01867e59a5ab60
game ardu-buggy ArduboyCollection/ArduBuggy 65ddafa84865a4130848f73b81a35b55be1d9683 \
    78ba2d1d585b3ee46261541982da58362bdf20df89ce71d900586d4be2d0b168
game ardu-man ArduboyCollection/arduman_arduboylib11 3c7fb4c28a3b7893d37c2de2df2640050226ec6e \
    5c05ef957dff9132430658b951f171ab371730fe240ec56650f9f1ec15e4676b
game back-to-the-jungle ArduboyCollection/ArduboyBackToTheJungle c3af5436e9f565091dd5f0b82f7597fa46a1cabf \
    a1724951ef665949024fa412d49dad0741bcc49a349e84a6456e5b1cb01de311
game boris-goes-skiing ArduboyCollection/boris-goes-skiing 75d7a11e2e16476e11346d04658014c766cb8b8e \
    ade29c06046744491bf6cce4b92470271fc9892c18aeba128ae0982d40405bd4
game diamonds ArduboyCollection/diamonds d8cd70dc657d0ee2520bd2b0a3d6d82b604320e6 \
    0e8c9c9221d9ec1784adf80dce7f7c4d8a6b009742c000671c5014045ecd8aa1
game evade ArduboyCollection/evade-arduboy-game 0933a8fb3cb78c083e38c7e37124ad3f08f3d90a \
    ceda8516dc9df5ac1bab79155b836db67fc0cdcf6abc79e313e529333c5a99ce
game kong Press-Play-On-Tape/Kong ec95141029a2a401edcf97b4b8d0f121139a2d15 \
    8b8f083bc8aa7d83671ea3fdb176eaf0725f80869a7d0628eb3572a305c619c8
game kong-ii Press-Play-On-Tape/Kong-II 8ba0b35b998d2c52161964ee06f126704bea1b13 \
    1b2527025a1c67a811dd939c13cc577ecdc2063aceec4a70f56ffaef7f0aee21
game loverush vampirics/LoveRush 993a2db547f0a5f36b971ba0846ad7030427dd47 \
    e8665be9a606e7179279196da95c97d0d3bd5fa464951a4096618f525cc15297
game pong ArduboyCollection/Pong-Arduboy 840b42a061cffde91a6ccfee701bde4a9859cc65 \
    b435673369e458ef9951bf26cf91c83b476901e3363adfd013f18fd99408c792
game poop-panic ArduboyCollection/poop-panic c0c44a89c897e1c7ab9026c7db74ef9a1bc51d84 \
    558837d13e38d7f133d643f69c018349bbac3b55679c0f1f30f9c23c5982810b
game ravine-despoiler unwiredben/arduboy-ravine-despoiler c48915c69c92c1f2e070775558866d84cd70b91b \
    2f1f6f9cd866eb7b549d2d9100098e9fc7b97f3e0cbe25e7cc10ab732726a55d
game space-cab vampirics/SpaceCab 928747e379fd91e95ba332b0200e9b1641e5b99c \
    cc34aa917aa3e7af9c93560e4832e88c4ca9fe958564613ab4ed311f87735033
game social-distance msanatan/TheSocialDistanceGame 025e8662092a5b553461d9688a053f33d9774d62 \
    946ca686d0c4cba872d05ec1bbaa7a813d2b2dc0f4adea92388dcd439569ad0a
game arduboy-life MLXXXp/ArduboyLife 4347faa12d3ef50373a15262a94f647a114883f5 \
    40c5c13030637bcb07c82107f9be77e4be6daf6155ea6882fa4600da59126450
game castleboy ArduboyCollection/CastleBoy d984802f15fb8435d73e803c584b879b0914ef37 \
    d580e764a6f0b1f9a805ab3d5bec4cae7845b510638834161a7833ce042aaafc
game the-bounce ArduboyCollection/TheBounceArduboy 2fcc65fffc3c3a213d8882aaa8ee7811fd738cdd \
    48b90fd7b6bfbb4a9a6f4f803c29f40f27af149d7562141bd74175616b87b2fd
game ardecipher databhor/ARDecipher 2b2255a5e8911d29e609bfb1e8b6e004f19e1e5e \
    1f085cb09372722d72a139eaf9576ec0e2ce5255faa5996e16bf789b23d784ed
game ardulo jonthysell/ArduLO 34d3b87a235dcedb682dcad9b9f95ed575650963 \
    6281d5a133bbf39b3fe55b9943514096666945a43bf7a302cfd2bf88e791ccd9
game box-stacker ArduboyCollection/BlockStacker 738dff9b59231227ab55bbb765c36069a839164a \
    f09f754ffe15603665caeadcaaac868483aac01750fddb730ad630f9d98dd30a
game dominoes Press-Play-On-Tape/Dominoes c094998ae445f24d7fead4d5f51e36e458704cc4 \
    0fda8b4e3481e56cad0ac7c0e40608ecde6788c68be12f01d02b13c29f70380b
game hangman serisman/arduboy-hangman da1825fb509b5b44d1fea67d931d4f535a240608 \
    282bf232fa13481c433424076b36e10c65c694085a5b16726a845fb790d8b7e8
game lite-out ArduboyCollection/ArduboyLiteOut 75a4e63b109559a7c979f6dd811d6974ab0665af \
    b2854e67d3844dfbbb24d27de056498dcb03395c970b15a545ba33036b7e51cd
game minesweeper Pharap/Minesweeper da9bb31c02ae67db48950e0032f0fb1b4a83d087 \
    f9b51d4245bd9cb358a6169afd4891c91339eb305d8bbe107f0d764b1eace04d
game pipeboy Glitsch3n/arduboy-game-collection b1ffab7e4f2591c93b18380d4ff6eb3011a71bac \
    18c27e782ed029cee25f4367c97a7b2e05ee8872d51bac94cc11717ab68b5703
game pipes ArduboyCollection/LayingPipe d3c0279079ef0a5c7161cbf39d2375e4d5a7aa25 \
    09113d09a7c39263d4dc2a2b5097012d965c722a4fff34942d0ab395d46a35ba
game ponghauki databhor/PongHauKi bcbc762078cc05db5d02458498eaea7a7be5d297 \
    ee93c97db42dc8190069a9c132f99ce8abcbcc7dd327a2c3ed04692cac015131
game ring-puzzle ArduboyCollection/ring_puzzle_game eee066c83ac6d4c769ee0062d04eabad48c7e074 \
    a215510d331aa278591ecb222da6b0d5f827e87a5a190b935de82e0204f6a917
game tictaccurly ArduboyCollection/TicTacCurly 9a5ffa170a4ce2c0fc875f616cf9edfc6ed19745 \
    f995834a813066646aed50a55cfdae8fd8839591e927ffc40bcb57d6e0808177
game tres ArduboyCollection/tres 8befbc9f536d2e0e8c61ae58d05e8f7840e1afdc \
    24a4c502e9c0823dbf0a32b4c3cea24c9ba59b4d72761ed81049f5153158346e
game waternet joyrider3774/waternet_arduboy 2f5e8ce47f9bb292a8c30fdf04f91988f0c2a300 \
    4a9ac49f0ffdf12c3294ed552c3c5239e05155c5fbeb32939053b344396d07f8
game ard-drivin ArduboyCollection/ard-drivin 78b6a0730878a622f3ca5b0c8fffd572507cd9b1 \
    8634eec95520801001077a7212c0988233afea921b0005ea4ea543c741e6f01a
game randocity pmwasson/Randocity 42e9b7e0fe0e17ee231f43577b222288306be238 \
    cabe58c63b537dd2f6e3cf6ca7d8394b9e1ce61035fd0f1ee1cad91a515d00ef
game dark-and-under ArduboyCollection/Dark-And-Under 44b209e85af7746ad10fd3dc0e8afaf9888d0f9a \
    87edd8782761cca21464bd1ddbf5e857c1a43b104107beea36350da93378bab4
game hello-commander ArduboyCollection/HelloCommander f540d0ab1f7c5386459569f92d536bebf7ed6345 \
    7480ff798c072a8c6d609f0320780ab33bb41901d8d036b45eee1864f183be25
game star-honor ArduboyCollection/StarHonor 0d1dfbe6cf6c30c1acda67cf7e405dc0d93845d5 \
    1497ec92e0ea9525c7400b5d91deaa32207b692caf64a67ea80470e0304621d9
game cosmicpods ArduboyCollection/CosmicPods 09239199ed828fad80a76707c4b7c170388abcf8 \
    fe3ea31fafb42009d9367a339c91d88df4d82fc64b84f271c7760ea9c6eb4a4e
game galaxion ArduboyCollection/galaxion 06abfe241d4cb8ae1937d42919a951f181e2f8dd \
    6badff6387372929cc908137cf216d763ae96f6c1ebc75da252a8c980d685fdb
game humanity-revenge ArduboyCollection/Humanity_Revenge_DC 9730bf5a7bcbe99cdfa65b966a9ebe156d88c02f \
    f36350389622f98b860c82d9ce9c251d9224efcb7de0c15eef49f7b6735dea48
game l4arduboy ArduboyCollection/i4arduboy 60e156f4a01d1db1a71d990f2577496eebd43bdb \
    652ce5a74fd9d558d6e178138d4d661e42866b16c4be2a2ae8a9c1e75f5feda8
game night-raid ArduboyCollection/night-raid d74fcf6912569806549b3411fbc647a85b43ccaa \
    668162c2048683476d8827f72d9c9b57196053d3ddf216bb49934cb87222a86f
game omega-chase Karl-Williams/OmegaChase 64bc7d38d21fe6684a147554b4b46b043fe6be4f \
    110656087cf435fbde5bc6e25c5dfc3d422e2151a04014ec4e519a028d3839f2
game stellar-impact ArduboyCollection/Stellar_Impact e56419cbbcf8e22e4633936d2acd0ddaf1bd87d6 \
    f1a7a1e142de72f8e5855708b81efe20e2996a94c912fef11848e39db6de8539
game chri-bocchi-cat obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game hollow-seeker obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game chie-magari-ita obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game knight-move obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game psi-colo obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game quarto obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game stairs-sweep obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
game ardubullets obono/ArduboyWorks d4b1f041789dcd1d71907654e4025d613b4ab420 \
    bac9eaa65042a669464f7fefc14eb12f85793c98457d73d094903d857a215a64
# GAMES-END
echo "arduboy sources ready in $dst"
