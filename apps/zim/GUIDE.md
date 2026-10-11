# zim

## A cosa serve

`zim` legge i file ZIM della libreria Kiwix: Wikipedia, Wikizionario, Wikivoyage, l'enciclopedia medica, le guide di riparazione di iFixit, la documentazione dei linguaggi di programmazione (DevDocs), i libri di Gutenberg e molto altro, senza Internet. Il catalogo ne conta migliaia, in tutte le lingue, aggiornati ogni mese.

Una voce esce come testo pulito: titolo, paragrafi, elenchi, tabelle in righe. Note, link di modifica e menu di navigazione restano fuori. ANIMA usa lo stesso programma quando una risposta è nella biblioteca.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `zim ls` | elenca le raccolte in `~/zim` |
| `zim search diabete` | i titoli che iniziano così, in tutte le raccolte (maiuscole indifferenti) |
| `zim get Insulina` | la voce intera come testo |
| `zim get 'Giuseppe Garibaldi' --intro` | solo l'introduzione |
| `zim get 'Diabete mellito' --sections` | l'elenco delle sezioni |
| `zim get 'Diabete mellito' --section Terapia` | una sezione sola |
| `zim get -z medicina Insulina` | cerca solo nella raccolta indicata (basta una parte del nome) |
| `zim get Insulina --max 2000` | al massimo 2000 caratteri |
| `zim get Insulina --all` | tiene anche note, bibliografia e collegamenti |
| `zim random` | l'inizio di una voce a caso |
| `zim info medicina` | titolo, lingua, data, numero di voci di una raccolta |
| `zim check medicina` | controlla che il file non sia danneggiato |

Si combina con la shell: `zim get Insulina > insulina.txt` salva la voce in un file, `zim get -z wikipedia img/foto.webp --out foto.webp` estrae un'immagine o un altro file.

## Dove prendere i file

Dal sito della libreria Kiwix (library.kiwix.org) si scaricano sul PC i file `.zim` che interessano, poi si copiano nella cartella `zim` della home della scheda SD (`/sdcard/home/zim`) con il lettore di schede. Per le schede FAT32 e per i file oltre 2 GB servono i file divisi in parti (`nome.zimaa`, `nome.zimab`…): `zim` li legge come un file solo.

Per cominciare vanno bene le versioni `nopic` (solo testo) o `mini` (solo le introduzioni): Wikipedia in italiano `top_nopic` pesa circa 900 MB, il Wikizionario circa 360 MB, l'enciclopedia medica circa 90 MB.

## Limiti

La ricerca trova i titoli che **iniziano** con le parole scritte; il testo completo delle voci non è ancora indicizzato. Se una voce non ha un titolo scritto come lo digiti e non ha un rinvio, prova con le maiuscole giuste o con `zim search`. Le immagini e le formule non si vedono nel Terminale: delle formule appare il codice TeX. Le raccolte fatte con zimit (copie di siti interi) danno risultati più grezzi.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
