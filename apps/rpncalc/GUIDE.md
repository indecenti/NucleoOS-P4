# RPN Calc

## Cos'è

Una calcolatrice in notazione polacca inversa, come le HP: prima i numeri, poi l'operazione. Niente parentesi.

## Come si usa

Scrivi un numero e premi ENTER: va nella pila. Scrivi il secondo e premi un'operazione: usa i due numeri in cima. Esempio, (3 + 4) × 5: `3 ENTER 4 + 5 ×`.

- SWAP scambia x e y, DROP toglie x, UNDO annulla l'ultima operazione, CLR svuota tutto, DEL cancella l'ultima cifra.
- STO e RCL salvano e richiamano la memoria; DEG/RAD cambia l'unità degli angoli.
- Tocca la pila a sinistra per passare a asin, acos e atan.

La pila e la memoria restano quando esci. Tastiera: cifre, Invio, + - * / ^, Backspace, Esc per azzerare.

## Licenza

NucleoOS, licenza MIT.
