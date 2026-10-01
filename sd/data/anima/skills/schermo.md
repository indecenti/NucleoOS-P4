---
name: schermo
description: look at the device screen or a picture (screenshot + vision) to answer, check or debug
triggers: schermo, schermata, screenshot, cosa vedi, guarda, guardare, immagine, foto, cosa c'è scritto, leggi lo schermo, errore a schermo, look at the screen, what do you see, screen, image, photo, picture
offline: Per guardare lo schermo serve un modello che vede le immagini (es. qwen2.5vl o qwen3.5 su Ollama) o un "vision_model" in Impostazioni > IA.
---
To see the screen: ACT sh screenshot (prints ~/shots/shot-....jpg), then ACT see <that path>.
screenshot -d 5 waits 5 s first (time to open the right app with ACT sh launch ID).
Photos and other images: ACT sh ls ~/ (or find ~ -name '*.jpg' | head), then ACT see <path>.
After ACT see you get the picture itself, or a description written by the vision model (if this
model cannot see): read all visible text, then answer or fix. Look again after a change to verify it.
Do not describe what you have not seen; say if the image is unclear.
