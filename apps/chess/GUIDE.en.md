# Chess

A game of chess against the computer. You play **white** and move first; the computer answers with
black.

## How to move

1. **Tap one of your pieces.** Its square lights up and the possible moves appear:
   - a **dot** marks an empty square you can go to;
   - **dark corners** mark an enemy piece you can capture.
2. **Tap the destination square.** The piece moves and the computer starts thinking.

You can also drag the piece with your finger and drop it on the destination square.
To pick another piece just tap it; tapping the same piece again deselects it.

The rules are the official ones: castling (move the king two squares towards the rook), *en passant*
and promotion. A pawn that reaches the last rank automatically becomes a **queen**.

The last move stays highlighted in yellow. If your king is in check, its square turns red.

## The buttons

- **Undo** — takes back your last move (and the computer's reply). You can use it several times,
  even after the game has ended.
- **New game** — starts over. While a game is in progress the button turns into **Confirm?**: tap
  it again within three seconds to really restart.
- **Level** — how strong the computer is. The change applies from its next move, even mid-game.

## The levels

| Level | What the computer is like |
|---|---|
| **Easy** | Looks only a little ahead and now and then plays a random move. Good for learning. |
| **Medium** | Plays solidly and punishes obvious mistakes. |
| **Hard** | Thinks longer (about two seconds) and calculates several moves ahead. |

While the computer thinks, three animated dots appear in the status box. You can still tap **Undo**
or **New game**: the computer stops at once.

## How a game ends

- **Checkmate** — the king in check has no way out: whoever gave mate wins.
- **Stalemate (draw)** — the side to move has no legal move but is not in check.
- **Draw: insufficient material** — not enough material is left to mate (for example king against
  king, or king and knight against king).
- **Draw by repetition** — the same position has occurred three times.
- **Draw: 50-move rule** — fifty moves each without a capture or a pawn move.

## The captured-pieces box

Below the status you see the pieces you (**You**) and the computer have captured. The number with
the **+** shows who is ahead in material (pawn 1, knight and bishop 3, rook 5, queen 9).

## Your game is saved

Every move is saved: close the app and reopen it and you find the game exactly where you left it,
with the level you chose.

## Tips

- In the first moves bring out the centre pawns, knights and bishops, then castle to keep your
  king safe.
- Before moving ask yourself: *which piece am I leaving undefended?* The computer notices at once.
- Made a mistake? **Undo** and try again: it is the quickest way to learn.
- If you win easily, move up a level.

---

The chess engine is [mcu-max](https://github.com/gissio/mcu-max) by Gissio (MIT license), based on
micro-Max by H.G. Muller.
