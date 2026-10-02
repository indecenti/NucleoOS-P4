# Eigenmath

## A cosa serve

Matematica simbolica: derivate, integrali, radici di polinomi, matrici, serie di Taylor e numeri esatti di qualsiasi lunghezza (frazioni, radicali, interi enormi). Dove la calcolatrice dà 0,333 Eigenmath risponde 1/3.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `eigenmath` | prompt `?`: scrivi un'espressione per riga, **EOF** per uscire |
| `eigenmath -e 'd(sin(x)^2,x)'` | derivata, risultato su una riga |
| `eigenmath -e 'integral(x^2*exp(x),x)'` | integrale indefinito |
| `eigenmath -e 'defint(x^2,x,0,1)'` | integrale definito |
| `eigenmath -e 'roots(x^2-5x+6)'` | radici esatte (`nroots` per quelle numeriche) |
| `eigenmath -e 'inv(((1,2),(3,4)))'` | matrice inversa (`det` per il determinante) |
| `eigenmath -e '212^17'` | interi esatti di qualsiasi grandezza |
| `eigenmath conti.txt` | esegue un file di espressioni dalla home |

Altre funzioni: `simplify`, `rationalize`, `float` (valore decimale), `taylor(f,x,n)`, `sum(k,1,10,f)`, `product(k,1,10,f)`. Si possono definire funzioni: `f(x)=x^2+1` e poi `f(3)`.

## Con Anima

È un programma di sistema: Anima lo usa da sola quando serve una risposta esatta. Puoi anche chiederle di usarlo, per esempio "quanto fa l'integrale di x^2 e^x?".

## Limiti

Non disegna grafici (`draw` non fa nulla). I risultati vengono sempre sviluppati: non c'è una funzione per fattorizzare. Al prompt i risultati sono disegnati su più righe (frazioni, matrici); con `-e` stanno su una riga.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
