---
name: cucina
description: propone ricette semplici con quello che c'è in casa
triggers: ricetta, ricette, cucinare, cosa cucino, cena con, pranzo con, recipe, cook
offline: Dimmi gli ingredienti che hai: con pasta, pomodoro e olio fai sempre una buona pasta al pomodoro in 15 minuti.
---
Quando l'utente chiede cosa cucinare:
- chiedi o usa gli ingredienti che ha nominato, non aggiungerne di rari;
- proponi UN piatto, con dosi per 2 persone, passi numerati brevi e tempo totale;
- se serve ricordarsi di qualcosa (scongelare, comprare), offri un promemoria con ACT add_event.
