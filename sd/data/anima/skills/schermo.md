---
name: schermo
description: see and use the device screen like a person (any app): read it, tap, type, check the result
triggers: schermo, schermata, screenshot, cosa vedi, guarda, guardare, immagine, foto, cosa c'è scritto, leggi lo schermo, errore a schermo, premi, tocca, clicca, attiva, disattiva, compila, impostazioni, look at the screen, what do you see, screen, image, photo, picture, tap, click, press, turn on, turn off
offline: Per usare lo schermo serve un modello in Impostazioni web > IA (per le immagini: uno che le vede, o un modello visivo).
---
Computer use on NucleoOS, one ACT per reply, check after every step:
1) ACT sh launch ID if the task is in an app (ACT sh apps lists them), else work where you are.
2) ACT sh ui: the screen as text, one line per item: [ref] role "text" @x,y (button, switch on/off,
   field, slider...). It is exact and cheap: prefer it to images.
3) Act: ACT sh input tap @REF (or tap "Text", or input tap X Y); for a field: tap it, then
   ACT sh input text TEXT and ACT sh input keyevent ENTER; scroll: ACT sh input swipe 512 450 512 150.
   After an action you get the new ui listing: verify the change happened before going on.
4) Pixels (colours, pictures, games, charts, an item ui does not list): ACT sh screenshot, then
   ACT see <the printed path>. You get the image, or a description from the vision model.
Stop and ask before anything irreversible (deleting, paying, sending messages, factory reset).
Photos and other images: find ~ -name '*.jpg' | head, then ACT see <path>.
Do not describe what you have not seen; say if something is unclear.
