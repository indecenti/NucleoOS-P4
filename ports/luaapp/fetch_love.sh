#!/bin/bash
# fetch_love.sh - the upstream LÖVE games of apps/love-* (ports/luaapp/LOVE_GAMES.md), pinned
set -euo pipefail
dst="$(cd "$(dirname "$0")/.." && pwd)/_src/love"
mkdir -p "$dst"; cd "$dst"
get() {   # owner/repo commit sha256
    local f="${1#*/}-$2.tar.gz"
    [ -f "$f" ] || curl -sSfL -o "$f" "https://github.com/$1/archive/$2.tar.gz"
    echo "$3  $f" | sha256sum -c --quiet -
    [ -d "${1#*/}-$2" ] || tar xzf "$f"
}
get Przemekkkth/love-2048 bb7fe3a14728ef548d4daaf06e1c3741b11ab827 \
    79d8b8940799a63666df7d03fa6bcecdfee7b7efa74080108d2801faf32e0999
get henriqueblang/minesweeper 6095210a51eec1f1a4028a4867c67c8851b48ecf \
    2b0fdc24fa93a8689f5f57fad22b1c5cf06f8869ac7993e55fdc1a3c37f1352e
get Przemekkkth/love-slide-puzzle 1e2b893f897bebbe12d129b699541428a90ddcc5 \
    832599b55fef11973af984ed9da276e3bf7eacd9f63ac7856cb5399ce868f522
get rafaeljacov/tictactoe_minimax c16b90b844125e0299865434c10728a1f9549e81 \
    33602b395c8c92090e9da74e854fe48cc71477363ed83e2ef79209d5cd64a0bd
get Przemekkkth/love-wormy 699c016499721ff021e9490f293a7511c21cbb4b \
    7aac45c4b3d67b86cacdcb0c0b443438a44da670d6403020e213f935100a2129
get whyboris/Gravity-Wars e66964fc33a37cc8d2a824ec4fad255d38c0d54f \
    7b9e9468cfc245d36a1adf84dca2425201bb8139f0570902e2dde201b29f2abb
get omrawaley/Moon-Pong ea35ba83cb68dc543d7c362d12e3ff74837f6083 \
    d3ed09777974532feb00a8e37e4901a5c91b26f8912b7ad14ae54a45a31dd701
get sleepycharlyy/fly_swatter.love 3f8a07117776e5bd32b9fc56fca7bab539f69292 \
    57694dc28989d2d7272c14800f7f421e2a3c783dd2d4dfa22c4b2d860211a7fe
get PR454D/panda-shooter 8cc402521034e76776ea46456fd23a611ec1e65a \
    5aba928139958c8afe2fe96269c7824fe0e5be3d5fbbe1a3fbe40d4457c2f3ed
get Foppygames/robo-house a84da633322284c8335c6857ca0a93430b347504 \
    ab174feb57ed78e7c49ba0eea42efb5a8e2b6a7f87aa64579df9b49fa3978dd4
get Shepardeon/total-breakout 8fb63da3b47ba6a087ddadb871aedf2558e31d74 \
    2b59c92e201f568c114abb44dd1aa0fc869a40417c68d45920f94bae9970caec
get chezrom/ubo 1c69fe290fb022e761145417387e1634fa8dc60a \
    0e7ae2c7eb1194fec301d00f9dbcf4eb284e4e6be54d12fc25ad3335b1bf0b39
get SimonLarsen/mrrescue a5be73c60acb8d1be506f7b5e48e784492ba96ce \
    2d739a277c6e34593e639e675eaa249c40bafd4214859f0d46033b8acfc21659
get Redoxee/CompressedGame 4f34833004478f6d626a1e7d2d338ba3693ea9a6 \
    892d73613b232493355e970dc87716304415bc5547e3f3e2f3dd125910d69f2a
get Foppygames/sword-of-the-orc-king d1a3adc094d448201c6d510365c7c6bd29a785d6 \
    f3aa3afa5d6d924aed93be6089fb5cc239fc9ef8f2f79888003d8aa37970ec97
get henriqueblang/checkers b7f2ce7ab99ab4cd2c2e0b12fd22995f26c98ec6 \
    87760e068f0b363fce8fe1c009e722891afed142fb78cff79a2c827914a4f6fe
