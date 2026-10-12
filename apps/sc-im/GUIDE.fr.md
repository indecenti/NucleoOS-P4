# sc-im, tableur

> **Le tableur pour le toucher, la souris et ANIMA est l'app Feuilles** (sur l'accueil) : sc-im est son moteur de calcul, livré avec le système. Ce guide s'adresse à qui préfère sc-im au clavier, dans le Terminal.

## À quoi il sert

sc-im est un vrai tableur, en plein écran : des cellules avec des nombres, des textes et des formules qui se recalculent toutes seules, des fonctions (`@sum`, `@avg`, `@max`, `@if`, dates et heures), des formats de nombre, la largeur des colonnes, le tri, les filtres, annuler et rétablir. Il ouvre les fichiers **Excel `.xlsx` et `.xls`**, **LibreOffice `.ods`**, **`.csv`** et **`.tsv`**, et enregistre en `.xlsx`, `.csv` et `.tsv` : une liste de dépenses, un inventaire, un échéancier, un classeur reçu par e-mail.

## Comment l'ouvrir

- **Touchez l'icône** sur l'Accueil : une feuille vide s'ouvre.
- Dans **Fichiers**, touchez un `.xlsx`, `.xls`, `.ods` ou `.csv` et choisissez **Ouvrir avec › sc-im**.
- Dans le Terminal : `sc-im depenses.xlsx`.

## Toucher et clavier

- **Touchez une cellule** pour la sélectionner. **Glissez vers le haut ou le bas** pour faire défiler la feuille.
- La rangée de touches au-dessus du clavier a **Échap**, **Tab**, **Ctrl** et les **flèches**.
- Avec un clavier USB ou KeyDeck, Ctrl, Maj et les touches de fonction marchent aussi.

## Les commandes principales

sc-im s'utilise comme l'éditeur vim : une lettre est une commande, les commandes longues commencent par deux-points.

| Touches | Ce qu'elles font |
|---|---|
| flèches | déplacent la sélection |
| `=` | saisir un nombre ou une formule, p. ex. `=@sum(B0:B9)` puis Entrée |
| `<` `>` `\` | saisir un texte aligné à gauche, à droite, centré |
| `e` / `E` | modifier le nombre / le texte de la cellule |
| `x` | effacer la cellule |
| `u` / Ctrl+R | annuler / rétablir |
| `yy` puis `p` | copier la cellule et la coller |
| `ir` `ic` | insérer une ligne / une colonne |
| `dr` `dc` | supprimer la ligne / la colonne |
| `f>` `f<` | colonne plus large / plus étroite |
| `f+` `f-` | plus / moins de décimales |
| `goB12` | aller à la cellule B12 |
| `:w` | enregistrer ; `:w comptes.xlsx` enregistre au format Excel |
| `:e csv comptes.csv` | exporter en CSV (aussi `tab`, `txt`, `mkd`) |
| `:load fichier.ods` | ouvrir un autre fichier |
| `:q` | quitter (`:q!` sans enregistrer) |
| `:h` | le manuel complet (`q` le ferme) |

Les cellules se nomment par colonne et ligne, **en comptant les lignes à partir de 0** : `A0` est la première cellule en haut à gauche. Dans les formules, les plages s'écrivent `A0:A9`.

## Avec Anima

sc-im est un programme du système : Anima s'en sert pour lire vos feuilles. Demandez par exemple « combien fait le total dans depenses.xlsx ? » ou « résume le tableau commandes.ods ».

## Limites

- Les graphiques, macros, images et commentaires des fichiers Excel ne sont pas lus ; d'un `.xlsx` on lit les valeurs, les textes et (avec l'option `--xlsx_readformulas`) les formules.
- Les fichiers `.ods` s'ouvrent mais s'enregistrent en `.xlsx`, `.csv` ou au format de sc-im (`.sc`).
- D'un ancien `.xls`, la première feuille est lue ; il s'enregistre en `.xlsx`.
- Le presse-papiers du système et les commandes `!` (programmes externes) ne sont pas disponibles.
- Les très grandes feuilles (des dizaines de milliers de lignes) peuvent ne pas tenir dans la mémoire de l'appareil : sc-im le signale et le fichier d'origine reste intact, car il enregistre d'abord dans un fichier temporaire.

## Dans le Terminal

- **Touchez l'icône** pour ouvrir le Terminal avec le programme déjà lancé, ou ouvrez **Terminal** et tapez la commande.
- Vos fichiers sont dans le dossier **home** de la carte SD (`/sdcard/home`) : pour le programme ce dossier est `/`. Copiez-y des fichiers depuis un PC en mettant la SD dans un lecteur.
- Lire une feuille sans l'ouvrir : `sc-im --nocurses --quit_afterload --export_csv depenses.xlsx` (avec `--export_mkd` on obtient un tableau).
- `apps` liste les programmes installés, `help sc-im` sa syntaxe.

---

*This product includes software developed by Andrés Martinelli <andmarti@gmail.com>.*
