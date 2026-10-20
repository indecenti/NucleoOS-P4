# WordGrinder, traitement de texte

## À quoi il sert

WordGrinder est un programme pour **écrire** : lettres, rapports, notes, chapitres. Il vous garde sur le texte, sans distractions, mais il a l'essentiel : titres, paragraphes, listes à puces et numérotées, **gras**, *italique*, souligné, rechercher et remplacer, comptage des mots, annuler et rétablir, plusieurs documents dans le même fichier. Il ouvre et exporte **OpenDocument `.odt`** (LibreOffice ; Word l'ouvre aussi), **Markdown**, **HTML** et texte brut ; il exporte aussi LaTeX, troff et Org.

## Comment l'ouvrir

- **Touchez l'icône** sur l'Accueil : un document vide s'ouvre.
- Dans **Fichiers**, touchez un `.odt`, `.md`, `.html` ou `.txt` et choisissez **Ouvrir avec › WordGrinder**.
- Dans le Terminal : `wordgrinder lettre.odt`.

Un `.odt`, `.md`, `.html` ou `.txt` est **importé** : à l'enregistrement, WordGrinder écrit un fichier `.wg` à côté (son format) et l'original n'est pas modifié. Pour le réécrire en `.odt`, utilisez **ESC › File › Export**.

## Toucher et clavier

- **Touchez le texte** pour y placer le curseur. **Glissez vers le haut ou le bas** pour faire défiler.
- **ESC** (dans la rangée de touches au-dessus du clavier) ouvre le **menu** : File, Edit, Style, Documents, Navigation. Chaque entrée a une lettre, et les plus utilisées ont un raccourci avec Ctrl.
- Avec un clavier USB ou KeyDeck, Maj+flèches (sélection) et Ctrl+flèches (mot par mot) marchent aussi.

| Touches | Ce qu'elles font |
|---|---|
| ESC | menu |
| Ctrl+S | enregistrer |
| Ctrl+O | ouvrir un document |
| Ctrl+B / Ctrl+U | gras / souligné (italique : ESC › Style › Set italic) |
| Ctrl+N | texte normal |
| Ctrl+P | style du paragraphe : titres H1–H4, listes, citations |
| Ctrl+Z / Ctrl+Y | annuler / rétablir |
| Ctrl+X / Ctrl+C / Ctrl+V | couper / copier / coller |
| Ctrl+F / Ctrl+K | rechercher et remplacer / suivant |
| Ctrl+G | aller à une page ou à un titre |
| Ctrl+Q | quitter |

Les menus de WordGrinder sont en anglais.

## Avec Anima

WordGrinder est un programme du système : Anima s'en sert pour lire un `.odt` ou transformer des notes en document. Demandez par exemple « lis-moi le fichier rapport.odt » ou « transforme notes.md en document odt ».

## Limites

- Les fichiers Word `.docx` et `.doc` ne s'ouvrent pas : enregistrez-les en `.odt`.
- Pas d'images, de tableaux, de colonnes ni de mise en page : c'est un programme pour le texte. Les images d'un `.odt` importé sont perdues.
- Le correcteur orthographique n'a pas de dictionnaires installés.
- Le presse-papiers est interne au programme.

## Dans le Terminal

- **Touchez l'icône** pour ouvrir le Terminal avec le programme déjà lancé, ou ouvrez **Terminal** et tapez la commande.
- Vos fichiers sont dans le dossier **home** de la carte SD (`/sdcard/home`) : pour le programme ce dossier est `/`. Copiez-y des fichiers depuis un PC en mettant la SD dans un lecteur.
- Convertir sans ouvrir : `wordgrinder --convert lettre.odt lettre.txt` (l'extension choisit le format : `.odt` `.md` `.html` `.txt` `.wg`, en sortie aussi `.tex`).
- `apps` liste les programmes installés, `help wordgrinder` sa syntaxe.
