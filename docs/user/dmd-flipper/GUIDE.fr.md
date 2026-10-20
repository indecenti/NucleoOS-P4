# DMD Flipper — Guide de l'utilisateur

**DMD Flipper** transforme votre carte NucleoOS (ESP32-P4 avec écran tactile 1024x600) en DMD de votre borne
de flipper virtuel et, quand aucune table ne tourne, en menu tactile de vos tables PinUP Popper.

Ce guide s'adresse au propriétaire de la borne : il explique comment relier la carte, comment installer le
petit programme « pont » sur le PC et comment utiliser chaque écran. Aucune connaissance en programmation
n'est nécessaire.

![L'horloge en points quand le PC se tait](../../screenshots/dmd_clock.jpg)

---

## Sommaire

1. [Ce que c'est et ce qu'il faut](#1-ce-que-cest-et-ce-quil-faut)
2. [Démarrage rapide en 5 étapes](#2-démarrage-rapide-en-5-étapes)
3. [Relier le PC](#3-relier-le-pc)
4. [Installer le pont pour PinUP Popper](#4-installer-le-pont-pour-pinup-popper)
5. [Le menu des tables](#5-le-menu-des-tables)
6. [Pendant la partie](#6-pendant-la-partie)
7. [Le panneau des réglages](#7-le-panneau-des-réglages)
8. [L'écran Composition](#8-lécran-composition)
9. [Dépannage](#9-dépannage)
10. [Mettre à jour le firmware en toute sécurité](#10-mettre-à-jour-le-firmware-en-toute-sécurité)
11. [Limites connues](#11-limites-connues)
12. [Confidentialité et réseau](#12-confidentialité-et-réseau)

---

## 1. Ce que c'est et ce qu'il faut

L'application fait deux choses :

- **Le DMD de la borne.** Pendant une partie, le PC (Visual Pinball X + VPinMAME + dmdext) envoie le DMD à la
  carte, qui le dessine en points LED/plasma. Les images peuvent arriver par **USB** ou par **Wi-Fi**, ou par
  les deux à la fois.
- **Un menu tactile pour PinUP Popper.** Quand aucune table ne tourne, la carte affiche vos tables (récentes,
  nouveautés, les plus jouées, favoris, toutes) avec wheels, backglass, statistiques et meilleurs scores :
  touchez **Jouer** et la table démarre sur la borne. Il faut pour cela le **pont**, un petit programme qui
  tourne sur le PC (chapitre 4).

### Ce qu'il faut

| Élément | Détails |
|---|---|
| Carte NucleoOS | Avec l'application **DMD Flipper**. La microSD est facultative : avec elle, la carte y garde les images et les dernières listes et le menu devient instantané ; sans elle le menu fonctionne quand même, mais les images passent par le câble à chaque fois et rien n'est conservé d'une session à l'autre. |
| Câble USB | Du port USB de programmation de la carte (Windows le voit comme le périphérique USB 303A:1001) **directement au PC**. Pas de hub non alimenté : la carte manquerait de courant et redémarrerait. |
| PC de la borne | Windows 10 (version 1803 ou ultérieure) ou 11, Visual Pinball X, VPinMAME avec **dmdext 2.x** (son `DmdDevice.dll`). |
| PinUP Popper | Pour le menu des tables, avec son **Web Remote** activé (chapitre 4). |
| Python | 3.8 ou ultérieur, pour le pont. |
| Wi-Fi | Facultatif pour le DMD (l'USB suffit), **conseillé** avec Popper : c'est par là que la carte quitte une table VPX, ou en lance une autre, pendant qu'on joue. |

---

## 2. Démarrage rapide en 5 étapes

1. **Reliez** la carte au PC avec le câble USB, directement (pas de hub non alimenté).
2. **Ouvrez DMD Flipper** sur la carte. Maintenez l'écran 2 secondes pour ouvrir le panneau et activez
   **Lancer au démarrage** : l'application doit déjà être ouverte quand une table démarre.
3. **Sur le PC**, dans le `DmdDevice.ini` de dmdext, activez la sortie USB :
   ```ini
   [zedmd]
   enabled = true
   ```
4. **Installez le pont** pour Popper (chapitre 4) : copiez le dossier, `pip install pyserial pillow`, ajoutez
   les deux lignes aux scripts des émulateurs dans Popper, puis
   `powershell -ExecutionPolicy Bypass -File ponte.ps1 installa`.
5. **Essayez** : sans table en cours, la carte affiche le menu. Choisissez une table, touchez **Jouer** : la
   table démarre et la carte passe au DMD. Quittez la table : le menu revient.

---

## 3. Relier le PC

La carte reçoit le DMD de deux façons, actives en même temps. Le choix se fait dans le `DmdDevice.ini` de
dmdext sur le PC (le fichier à côté du `DmdDevice.dll` de VPinMAME). La page d'attente de l'application montre
les deux configurations, avec l'adresse exacte de votre carte.

### USB (conseillé)

```ini
[zedmd]
enabled = true
```

- La carte se présente à dmdext comme un **ZeDMD 128x32** : dmdext parcourt les ports série et la trouve tout
  seul. Pour fixer le port, ajoutez `port = COMx` sous `[zedmd]` (le numéro de port COM de la carte dans le
  Gestionnaire de périphériques ; `ponte.ps1 stato` l'affiche à la ligne « Scheda »).
- **L'application doit être ouverte au démarrage de la table** : c'est à ce moment que dmdext cherche le
  périphérique. D'où l'intérêt de **Lancer au démarrage** (ci-dessous).
- Les DMD d'autres tailles (par exemple 128x16 ou 192x64) sont ramenés à 128x32 par dmdext.
- Avec le pont pour Popper, l'USB est aussi le canal du menu : le pont et dmdext se passent le port à chaque
  début et fin de partie, tout seuls.

### Wi-Fi

```ini
[networkstream]
enabled = true
url = ws://<IP de la carte>:9090/dmd
retry = true
```

- L'adresse exacte (`ws://…:9090/dmd`) est écrite sur la page d'attente de l'application et en haut du panneau
  des réglages, sous la ligne d'état.
- En Wi-Fi, la carte affiche le DMD à sa vraie taille, jusqu'à 512x256.
- Un seul PC à la fois : une nouvelle connexion remplace la précédente.
- Le port 9090 se change dans le panneau (**Port réseau**) ; avec `retry = true` le PC se reconnecte tout seul.

> **Astuce — IP fixe.** Dans votre box, donnez à la carte une adresse fixe (réservation DHCP) : l'URL du
> `DmdDevice.ini` ne changera jamais.

### Lancer au démarrage

Dans le panneau (section **Système**), activez **Lancer au démarrage** (désactivé d'origine) : une seconde après
la mise sous tension, NucleoOS ouvre directement DMD Flipper. Si l'écran de verrouillage est actif,
l'application ne s'ouvre pas seule tant que vous n'avez pas déverrouillé : sur une borne, mieux vaut ne pas
utiliser le verrouillage.

---

## 4. Installer le pont pour PinUP Popper

### Ce qu'il fait

Le pont (`nucleo_bridge.py`) tourne sur le PC de la borne, démarre avec Windows et est très léger (priorité
basse, environ 27 Mo, aucune fenêtre). Tout le travail graphique est fait par la carte.

- **Quand aucune table ne tourne**, il envoie à la carte les listes de tables, les fiches des tables et les
  images (converties une seule fois et gardées en cache).
- **Quand une table VPX démarre**, il cède le port USB à dmdext : la carte devient le DMD. En fin de partie, il
  reprend le port.
- **Tables Future Pinball avec DMD** : le pont le recopie sur la carte avec le mode mirror de dmdext.
- **Pinball FX, MAME, jeux PC** : le pont garde le port et la carte affiche la vitrine du jeu en cours, avec les
  actions rapides.
- **Le lancement et la sortie** des tables passent par le Web Remote de Popper.
- Il lit la base de données de Popper (`PUPDatabase.db`) **en lecture seule** : il n'y écrit jamais rien. Les
  tables masquées dans Popper n'apparaissent pas.
- Si Popper ne signale pas la fin d'un jeu (émulateur planté ou fermé de force), le pont s'en aperçoit seul
  quand le processus de l'émulateur se termine, et le menu revient.
- Il ferme le serveur de backglass B2S que VPX laisse parfois ouvert après un plantage (sinon Popper ne
  lancerait plus rien).
- On peut le redémarrer même pendant une partie : il reconnaît la table en cours.

### Prérequis

- Python 3.8 ou ultérieur, avec `pip install pyserial pillow` (numpy est facultatif : il accélère les images).
- PinUP Popper, avec le **Web Remote** activé :
  - `useWEB = true ;` dans `PUPMenuScriptSysOptions.txt` et dans `PuPMenuScript.pup` (les « options de script »
    de Popper), puis redémarrez Popper ;
  - le composant COM `PuPServer.WebServer` enregistré (sinon : `PUPServerRegisterCOM.bat` en administrateur) ;
  - port 80 (si Popper utilise un autre port, lancez le pont avec `--web-port`).
  Le Web Remote sert pour **Jouer**, pour quitter une partie et pour la vitrine.
- dmdext avec `[zedmd] enabled = true` dans le `DmdDevice.ini`.
- Sur la carte : l'application **DMD Flipper** ouverte (de préférence avec **Lancer au démarrage**).

### Installation

1. Copiez le dossier `popper_bridge` sur le PC, par exemple dans `C:\vPinball\nucleo_bridge`.
2. Installez les bibliothèques :
   ```
   pip install pyserial pillow
   ```
3. Vérifiez qu'il trouve tout :
   ```
   python nucleo_bridge.py --check --report report.txt
   ```
   Le rapport indique ce qu'il trouve : dossier de Popper, colonnes de la base, listes, médias, Web Remote,
   port de la carte, `DmdDevice.ini`.
4. Dans **Popper Setup → Emulators**, pour **chaque** émulateur visible, ajoutez en **première** ligne :
   - dans le **Launch Script** :
     ```
     curl -s -m 2 -G --data-urlencode "name=[GAMEFULLNAME]" http://127.0.0.1:58090/game/start >nul 2>&1
     ```
   - dans le **Close Script** :
     ```
     curl -s -m 2 http://127.0.0.1:58090/game/end >nul 2>&1
     ```
   `curl` est fourni avec Windows 10 depuis la version 1803. Si le pont ne tourne pas, ces lignes ne bloquent
   rien : elles échouent en 2 secondes.
5. Faites démarrer le pont avec Windows (depuis le dossier du pont) :
   ```
   powershell -ExecutionPolicy Bypass -File ponte.ps1 installa
   ```
   Cela crée une tâche planifiée à l'ouverture de session Windows, sans fenêtre, en priorité basse, relancée si
   elle se ferme ; et démarre le pont tout de suite.
6. Essayez : sans table en cours, la carte affiche le menu. Touchez une table, puis **Jouer**.

### Commandes de `ponte.ps1`

Les commandes sont des mots italiens. Lancez-les ainsi, depuis le dossier du pont :
`powershell -ExecutionPolicy Bypass -File ponte.ps1 <commande>`

| Commande | Ce qu'elle fait |
|---|---|
| `installa` (installer) | Démarrage avec Windows (tâche planifiée) et démarrage immédiat |
| `rimuovi` (retirer) | Retire le démarrage avec Windows et arrête le pont |
| `avvia` (démarrer) | Démarre le pont |
| `ferma` (arrêter) | Arrête le pont (et un dmdext qu'il aurait lancé) : le port de la carte est libre |
| `riavvia` (redémarrer) | Arrête puis redémarre |
| `stato` (état) | Allumé/éteint, CPU et mémoire, démarrage avec Windows oui/non, port COM de la carte, dernières lignes du journal |
| `controllo` (contrôle) | Lance le contrôle (`--check`) et enregistre le rapport dans `%LOCALAPPDATA%\nucleo_bridge\controllo.txt` |

Les messages du script sont en italien (« Ponte ACCESO » = pont allumé, « SPENTO » = éteint, « Scheda » = carte).

- Journal : `%LOCALAPPDATA%\nucleo_bridge\ponte.log` (au-delà de 5 Mo il est déplacé dans `ponte.log.1`).
- État depuis le navigateur du PC : `http://127.0.0.1:58090/status` indique si le pont a le port USB et si une
  partie est en cours.
- Options à la main : `python nucleo_bridge.py [--popper DOSSIER] [--port COMx] [--web-port 80]`.
  `--popper` est nécessaire si Popper n'est pas dans un dossier standard (`C:\vPinball\PinUPSystem`,
  `C:\PinUPSystem`, `D:\vPinball\PinUPSystem`).

### Mettre à jour le pont

1. `ponte.ps1 ferma`
2. Copiez les nouveaux fichiers dans le dossier du pont (les caches restent dans `%LOCALAPPDATA%\nucleo_bridge`).
3. `ponte.ps1 avvia`

`installa` ne sert que la première fois, ou si la tâche planifiée a été supprimée.

### L'arrêter ou le retirer

- Pour un moment : `ponte.ps1 ferma` (il redémarre à la prochaine ouverture de session Windows, ou avec `avvia`).
- Définitivement : `ponte.ps1 rimuovi`, puis retirez les deux lignes `curl` des scripts des émulateurs dans
  Popper.

---

## 5. Le menu des tables

Sans table en cours et avec le pont allumé, la carte est un menu Popper. En haut, les listes et trois boutons ;
au centre, la table sélectionnée ; en bas, la barre des wheels.

### Les deux styles

À choisir dans le panneau, **Style du menu** :

| Style | Aspect |
|---|---|
| **Cabinet** (par défaut) | Comme le thème par défaut de Popper : la barre des wheels entre des rails chromés avec un tube lumineux cyan, la wheel choisie dans une sphère de verre chromée, **Jouer** en cyan allumé. |
| **DMD** | Titres, infos, listes et boutons dessinés en points colorés comme un ColorDMD, le backglass vu à travers une grille de LED, cadres carrés. Le texte en points est en majuscules. |

### Les listes

| Liste | Ce qu'elle contient |
|---|---|
| **Récentes** | Les tables jouées, de la plus récente à la plus ancienne |
| **Nouveautés** | Toutes les tables, d'abord les dernières ajoutées dans Popper |
| **Les plus jouées** | Les tables jouées au moins une fois, par nombre de parties |
| **Favoris** | Les favoris marqués dans Popper (si la liste est vide : « Pas encore de favoris : marquez-les dans Popper. ») |
| **Toutes** | Toutes les tables visibles, par ordre alphabétique |

En haut à droite, le compteur (par exemple « Visual Pinball X 28 / 72 ») et trois boutons :

- **A-Z** — ouvre **Aller à** (ci-dessous) ;
- **aléatoire** (icône à flèches croisées) — **Surprends-moi** : une table au hasard ;
- **engrenage** — le panneau des réglages du DMD (chapitre 7).

### La barre des wheels

- **Faites glisser** la barre : la wheel du milieu, plus grande et éclairée, est la table sélectionnée. La barre
  tourne en boucle.
- **Touchez** une wheel : elle vient au milieu.
- **Lancez-la** d'un geste rapide : la barre continue seule, d'autant plus loin que le geste est rapide (au
  plus un tiers de la liste) ; un glissement lent avance de la distance parcourue.
- **Balayez** vers la gauche ou la droite sur la zone centrale : une table en avant ou en arrière.
- **Au clavier ou à la télécommande** : les flèches gauche/droite déplacent la barre, Entrée active le bouton
  sélectionné (d'abord **Jouer**), Échap / Retour ferme d'abord les fenêtres ouvertes (Aller à, instructions,
  confirmations).

Une table sans wheel affiche son nom à la place de l'image.

### Aller à (A-Z)

- En haut, les **catégories** de la liste ouverte, dans la langue de la carte (Visual Pinball X, Future Pinball,
  Pinball FX, Shoot'em up, Labyrinthes, Plates-formes, Sport et course, Autres arcade, Jeux PC), chacune avec le
  nombre de tables de **cette** liste qu'elle contient ; la première, **Tous les jeux**, retire le filtre. Une
  catégorie filtre la liste.
- En dessous, les **lettres** : une lettre saute à la première table qui commence par elle. Les lettres par
  lesquelles aucune table ne commence sont éteintes ; chiffres et symboles sont **0-9**.
- Pour fermer : le **✕**, un toucher hors du cadre, un balayage vers le bas ou Retour.

### Surprends-moi

Le bouton aléatoire affiche une table au hasard. Sa fiche propose alors **Une autre** pour en tirer une autre
aussitôt.

### La fiche de la table

La table sélectionnée remplit l'écran : son backglass derrière (assombri), sa grande wheel, son nom, fabricant
et année, parties et temps de jeu (« 10 parties, 7 h 16 min de jeu »), la dernière partie (dates au format
jj/mm/aaaa), **Favori** si c'en est un, les meilleurs scores. Les boutons :

| Bouton | Ce qu'il fait |
|---|---|
| **Jouer** | Lance la table sur la borne (via le Web Remote de Popper) |
| **Instructions** | Seulement si la table a une carte d'instructions dans Popper : l'affiche redressée, sur toute la largeur ; faites défiler du doigt, touchez (ou Retour) pour fermer. Tant que la page n'est pas arrivée elle affiche « Chargement... » ; elle se ferme aussi avec le **✕**. Si la page se perd, la carte la redemande deux fois, puis la ferme avec « Les instructions ne sont pas arrivées du PC : réessayez. » ; si le PC n'a pas d'instructions pour cette table : « Pas d'instructions pour cette table. » |
| **Composition** | Comment la table utilise le moniteur du PC et cet écran (chapitre 8), sans avoir à la jouer |
| **Une autre** | Après Surprends-moi : une autre table au hasard |

Après **Jouer** s'affiche « Lancement... » avec le nom de la table ; **Retour** masque seulement cette page. Si
Popper n'a rien lancé au bout d'environ 2 minutes (la carte attend jusque-là : le pont réessaie tant que Popper refuse les lancements, par exemple juste après un plantage) : « La table n'a pas démarré : vérifiez le Web Remote de
Popper. »

### Vitrine : quand vous utilisez les boutons de la borne dans Popper

Quand quelqu'un utilise le joystick/les boutons de la borne dans Popper, la carte suit sa sélection et affiche
la **vitrine** de la table : backglass, DMD et topper l'un après l'autre, toutes les 7 secondes, chacun dans un
halo de ses propres couleurs, avec « Touchez pour choisir une table ». Un toucher ouvre le menu sur cette table.

- Les flyers et les instructions ne sont pas dans la vitrine (dans Popper ce sont des pages verticales
  enregistrées couchées) : les instructions s'ouvrent avec le bouton **Instructions**.
- Si Popper sélectionne une entrée qui n'est pas une table (« Go Back », « Exit », une liste de lecture), la carte
  affiche son action en grand (par exemple « RETOUR », « QUITTER », ou le nom de la liste) et dessous
  « Sur Popper : … ».
- Quand Popper revient à son mode attract, le menu de la carte revient.
- Si une table est lancée depuis Popper (et non depuis la carte), la carte passe quand même au DMD.

### Mode attract

Après 2 minutes sans toucher l'écran, la barre tourne seule (une table toutes les 8 secondes) et « Touchez pour
choisir » clignote. Cela n'arrive jamais pendant une partie ; à la fin d'une partie les 2 minutes repartent de
zéro, et la table qu'on vient de jouer reste à l'écran avec ses scores à jour.

---

## 6. Pendant la partie

### Ce que montre la carte

| Situation | À l'écran |
|---|---|
| Table VPX avec DMD | Le DMD en points, selon la disposition choisie (chapitre 7) |
| Table VPX qui n'a pas encore envoyé son DMD | « Chargement... » jusqu'à une minute, puis la page du jeu (« En jeu depuis 12 min ») : c'est une table sans DMD, par exemple une alphanumérique (voir chapitre 9) |
| Table Future Pinball avec DMD | Le DMD, recopié par le pont |
| Pinball FX, MAME, jeux PC | La vitrine du jeu en cours |
| PC silencieux depuis 5 s (sans Popper) | L'horloge en points, en 12 ou 24 heures comme l'horloge du système, si **Horloge quand le PC se tait** est activé |

Tant que le DMD d'une partie est à l'écran, les gestes système de NucleoOS (volet, Accueil, Récents) sont
bloqués : un effleurement ne peut pas masquer le score.

### Gestes et touches

- **Toucher bref** sur le DMD (ou sur la vitrine d'un jeu sans DMD, ou sur la page « En jeu » d'une table qui n'a pas encore envoyé son DMD) : les **actions rapides**.
- **Maintenir 2 secondes** (ou Entrée / Espace au clavier, aussi sur l'horloge et la page d'attente) : le
  **panneau des réglages**. Panneau ouvert, les gestes système fonctionnent de nouveau.
- **Retour** (ou Échap) sur le DMD ouvre et ferme le panneau : il ne ferme jamais l'application.

### Actions rapides

| Élément | Ce qu'il fait |
|---|---|
| **Retour au DMD** | Ferme les actions rapides |
| **Tables** | Parcourir les tables pendant la partie ; en haut, un bouton **Retour au DMD** |
| **Quitter la partie** | Ferme la table en cours (« Fermeture de la partie... ») et le menu revient |
| **Réglages du DMD** | Ouvre le panneau des réglages |
| **Composition de la table** | La composition de la table en cours (chapitre 8), ouverte sur l'onglet **Cet écran** |

Pour fermer les actions rapides sans choisir : touchez hors du cadre ou balayez vers le bas.

Pour les tables VPX, la sortie passe par le **Wi-Fi**, directement vers le Web Remote de Popper (à ce moment le
câble appartient à dmdext) : la carte doit être sur le même réseau que le PC. Si le PC ne répond pas : « Le PC ne
répond pas en Wi-Fi : quittez la partie sur la borne. » Pour les autres émulateurs, la sortie passe par le
câble, via le pont. Si la partie ne se ferme pas après les nouvelles tentatives (y compris en changeant de table) :
« Le PC n'a pas fermé la partie : quittez-la sur la borne. »

### Lancer une autre table

Depuis **Tables**, choisissez une table et touchez **Jouer**. La carte demande « *X* est en cours. Le quitter et
lancer *Y* ? » : **Quitter et lancer** ferme la table en cours et lance la nouvelle dès que le pont signale la
fin ; **Annuler** laisse tout en l'état.

### Quand le câble sert au DMD

Pendant une partie dont le DMD passe par dmdext (les tables VPX), le câble est occupé par le DMD : **Surprends-moi**, les fiches des autres tables, les **Instructions** et le choix de la version du backglass affichent « Disponible en fin de partie : pendant le jeu, le câble sert au DMD. »

### Une table qui se ferme toute seule

Si la table plante ou est fermée de force (sans l'avis de fin de Popper), la carte le signale — « *Nom* s'est fermé tout seul : vous pouvez le relancer d'ici. » — et la laisse sélectionnée, prête pour **Jouer**.

---

## 7. Le panneau des réglages

Il s'ouvre en maintenant l'écran 2 secondes (ou avec Entrée / Espace / Échap), depuis l'engrenage du menu, ou
depuis **Réglages du DMD** dans les actions rapides. En haut :

- **la ligne d'état** : USB / Wi-Fi, le format en clair (« 16 nuances », « 16 couleurs », « RGB », « horloge »,
  « Écran de test », « afficheurs de score »), taille, images par seconde, jeu ;
- **dessous, l'adresse Wi-Fi** à mettre dans le `DmdDevice.ini` (`ws://…:9090/dmd` ; « pas encore d'adresse »
  si la carte n'est pas sur un réseau) ; quand le pont pour Popper a salué la carte, la même ligne affiche aussi « Pont <version> · Popper : prêt » (ou « base de données introuvable ») ;
- trois boutons : **Retour au DMD** (il devient **Retour** quand aucun PC ne pilote le DMD) ferme le panneau ;
  **Écran de test** (ci-dessous) ; **Fermer le DMD** ferme l'application, après la question décrite ci-dessous.

Chaque modification s'applique tout de suite et reste enregistrée dans la carte.

### Fermer l'application

Toute façon de quitter DMD Flipper — **Fermer le DMD**, les gestes Accueil / Récents, les touches système, le bouton de fermeture de la fenêtre en mode bureau classique —
demande d'abord **« Fermer le DMD ? »** : pendant une partie « Une table est en cours : le flipper n'aura plus de
DMD jusqu'à ce que l'app soit rouverte. », sinon « Le flipper n'aura plus de DMD ni de menu des tables jusqu'à ce
que l'app soit rouverte. » **Rester** (le bouton déjà sélectionné) annule ; **Fermer** quitte. Retour ne ferme
jamais l'application.

### Écran

| Réglage | Ce qu'il fait | Par défaut |
|---|---|---|
| **Disposition pour toutes les tables** | Comment le DMD occupe l'écran (tableau ci-dessous). Le cadre affiche le nom et la description de la disposition ; si la table en cours a la sienne, il l'indique (« Cette table a la sienne : … »). Touchez le cadre ou **Choisir avec les aperçus** pour choisir (chapitre 8). | DMD classique |
| **Rotation** | 0°, 90°, 180°, 270° dans le sens horaire, selon le montage de la carte | 0° |

Les dispositions, dans l'ordre des aperçus :

| Disposition | Ce qu'elle fait |
|---|---|
| **Automatique** | Pour chaque table la meilleure disposition selon ses images : Décor avec fenêtre s'il a le FullDMD, Backglass avec logement s'il a la grille, sinon DMD classique. Un DMD 128x16 prend DMD plein écran (lignes reconstruites). |
| **DMD plein écran** | Tout l'écran sans vide : les lignes sont reconstruites (128x64) et les points restent ronds et grands. |
| **DMD classique** | Points carrés et vraies proportions, aussi grands que possible ; bandes en haut et en bas. |
| **DMD haut** | Le DMD sur toute la largeur et un peu plus haut ; bandes en haut et en bas. |
| **DMD étiré** | Tout l'écran avec les points d'origine : l'image s'étire, les points restent petits et les lignes espacées. |
| **Backglass + DMD** | Le backglass de la table en haut et le DMD dessous, comme dans un vrai fronton. Sans backglass, le DMD est centré. |
| **Backglass seul** | Seulement le backglass de la table, sans DMD : la carte sert d'écran de backglass. |
| **Décor de la table** | Le décor FullDMD de la table en haut et le DMD dans une large bande dessous. |
| **Décor avec fenêtre** | Le décor FullDMD de la table sur tout l'écran et le DMD dans sa fenêtre. |
| **Backglass avec logement** | Le backglass avec sa grille sur tout l'écran et le DMD dans son logement. |

- Les dispositions avec backglass ou décor utilisent les images que le pont envoie au début de la partie : sans
  pont, ou pour une table qui ne les a pas, elles ne sont pas disponibles.
- **Écran pivoté** (90°, 180°, 270°) : les dispositions avec images affichent le DMD classique ; à 90° et 270°
  aussi DMD haut et DMD plein écran. Le panneau et l'écran Composition l'indiquent : « Écran pivoté : les
  dispositions avec images affichent le DMD classique. »
- L'horloge occupe toujours tout l'écran en points ronds, quelle que soit la disposition.

### Menu des tables

| Réglage | Valeurs | Par défaut |
|---|---|---|
| **Style du menu** | **Cabinet** / **DMD** (chapitre 5) | Cabinet |

### Points

| Réglage | Ce qu'il fait | Par défaut |
|---|---|---|
| **Couleur** | La couleur des DMD monochromes, choisie dans une rangée de cercles colorés ; la légende nomme celle choisie (« Couleur · Orange »). L'anneau vide est **Du PC** (la couleur envoyée par le PC) ; puis Orange, Rouge, Jaune, Vert, Cyan, Blanc. Cela marche aussi en USB : les images monochromes sont recolorées, tandis que les DMD en couleur (ColorDMD, Serum, palettes) gardent leurs couleurs ; **Du PC** garde la couleur de dmdext. | Orange |
| **Luminosité** | De 10 à 100 %, au doigt ou au clavier ; enregistrée dans les deux cas | 100 % |
| **Forme du point** | **Rond** ou **Carré** | Rond |
| **Halo** | Un léger halo lumineux dans les espaces entre les points | Désactivé |

### Système

| Réglage | Ce qu'il fait | Par défaut |
|---|---|---|
| **Horloge quand le PC se tait** | Si le PC n'envoie rien pendant 5 secondes (après avoir affiché quelque chose), la carte affiche l'heure en points (12 ou 24 heures, comme l'horloge du système) avec la date dessous. Elle n'apparaît jamais par-dessus le menu Popper ni pendant une partie. | Activé |
| **Lancer au démarrage** | NucleoOS ouvre DMD Flipper à la mise sous tension | Désactivé |
| **Port réseau** | Le port pour le Wi-Fi. Il s'applique quand vous validez ou fermez le panneau, et n'est enregistré qu'une fois la carte réellement à l'écoute dessus. Les erreurs s'affichent sous le champ (« Port : un nombre de 1 à 65535. », « Impossible d'ouvrir le port … ») : le port en service reste alors en place. | 9090 |

### Écran de test

Pendant 10 secondes la carte affiche « TEST » avec un compte à rebours des secondes, la couleur choisie en 16
niveaux, des barres de couleur (rouge, vert, bleu, jaune, cyan, magenta, blanc, gris), une bordure et un bloc
plein dans le coin supérieur gauche. Pendant ce temps les images du PC sont ignorées. Un toucher, Entrée, Espace
ou Échap y mettent fin plus tôt. Il sert à vérifier l'orientation, les couleurs et la luminosité de la borne
**sans PC** : si le bloc n'est pas en haut à gauche, changez la **Rotation**.

---

## 8. L'écran Composition

Vous y choisissez, avec des aperçus faits à partir des vraies images de la table, comment une table utilise les
écrans de la borne. Il s'ouvre de trois façons :

| Depuis | Ce qu'il règle |
|---|---|
| Panneau → **Choisir avec les aperçus** | La disposition de **cet écran** pour **Toutes les tables** : les dix dispositions dans une grille, toutes visibles. Les aperçus utilisent les images de la table en cours, ou de celle sélectionnée dans le menu. |
| Fiche de la table → **Composition** | Le moniteur du PC et cet écran pour **cette** table, sans avoir à la jouer |
| Actions rapides → **Composition de la table** | La même chose, pour la table en cours ; il s'ouvre sur l'onglet **Cet écran** |

Le moniteur du PC se choisit table par table : depuis le panneau, on ne choisit que cet écran. Fermez avec le
**✕** en haut à droite ou avec Retour. En haut, la ligne « Images de cette table : … » indique ce que possède la
table (backglass et sa taille, avec grille, décor FullDMD, backglass double) ; pendant qu'elles arrivent :
« Les images de la table arrivent du PC... ».

### Onglet « Cet écran »

- **Vrais points, 1:1** : une loupe qui montre le milieu du DMD dessiné à la taille réelle de l'écran, pour juger
  la taille des points et les espaces entre les lignes avant de sortir (une miniature ne peut pas les montrer).
- Le nom de la disposition choisie et ce qu'elle fait. Pour **Automatique** sur une table qui n'est pas en
  cours : « décidée au lancement de la table ».
- **Taille des points** : **Ajustés** (à la disposition) ou **D'origine** (carrés, en pixels entiers, jamais
  étirés).
- **Marge** : 0, 1, 2 ou 3 lignes et colonnes de points éteints autour du DMD.
- Seulement en partie, avec un DMD 128x16 : **DMD 128x16 : reconstruit** (porté à pleine hauteur, par défaut) ou
  **DMD 128x16 : d'origine**.
- Les dispositions en cartes : pour une table, la première est **Comme toutes les tables** (suit le choix du
  panneau), puis seulement celles possibles avec les images de la table.

### Onglet « Second écran (PC) »

Décide de ce qu'affiche le **moniteur de backglass du PC** pour cette table. Le pont écrit le choix là où B2S et
dmdext le lisent au démarrage de la table (le fichier `.res` de la table, `B2STableSettings.xml`, la section du
jeu dans le `DmdDevice.ini`), après avoir fait **une copie de sauvegarde** de chaque fichier avec le suffixe
`.prima-di-s2`.

| Choix | Sur le moniteur du PC |
|---|---|
| **D'origine** | Comme le place le fichier du backglass de la table (B2S), sans changement |
| **Backglass** | Seulement le backglass, sur tout le moniteur |
| **Backglass + DMD** | Le backglass en haut et le DMD dessous |
| **Backglass avec grille** | Le backglass entier avec sa grille de haut-parleur |
| **Décor FullDMD** | Le décor FullDMD de la table (la deuxième image, plus petite, du fichier B2S) |
| **Backglass + décor** | Le backglass en haut et dessous le décor FullDMD |

Seuls les choix possibles pour cette table apparaissent. En plus :

- **DMD aussi sur le moniteur** : **Non** (le DMD reste uniquement sur cette carte) ou **Oui** (le DMD virtuel de
  dmdext aussi sur le moniteur). Avec **D'origine** ces boutons sont désactivés : « Choisissez d'abord une des
  compositions ci-dessous. »
- **Version du backglass** : **Authentique** ou **Fantaisie**, pour les backglass doubles : B2S allume l'autre
  série d'images dès la prochaine partie.
- Les changements s'appliquent **à la prochaine partie** (« Second écran prêt pour la prochaine partie »). Si vous
  êtes en train de jouer, **Relancer la table** apparaît pour la fermer et la rouvrir aussitôt avec la nouvelle
  composition.
- « Pas de second écran sur le PC » : le pont n'a trouvé aucun second moniteur. « Cette table n'a pas de fichier
  backglass (B2S) » : le moniteur montre le backglass de Popper et il n'y a rien à choisir. « Le PC n'a pas répondu : impossible de décrire le fichier du backglass. » : le PC n'a pas décrit le fichier du backglass après 3 tentatives ; rouvrez la Composition.

---

## 9. Dépannage

| Problème | Que faire |
|---|---|
| **Pas de DMD sur la carte** | Vérifiez que l'application est ouverte **avant** de lancer la table ; que le `DmdDevice.ini` contient `[zedmd] enabled = true` (USB) ou `[networkstream]` avec la bonne URL (Wi-Fi) ; ouvrez le panneau et regardez la ligne d'état (USB / Wi-Fi, images par seconde). |
| **dmdext indique « ZeDMD is not available »** | L'application n'était pas ouverte au démarrage de la table, ou un autre programme occupe le port COM. Activez **Lancer au démarrage**, fermez les programmes qui utilisent le port et, si besoin, fixez le port avec `port = COMx` sous `[zedmd]`. |
| **La carte redémarre au lancement d'une table** | En ouvrant le port, dmdext envoie à la carte un signal de réinitialisation : l'application le bloque **seulement tant qu'elle est ouverte**. Gardez-la ouverte (Lancer au démarrage). Si elle redémarre quand même, retirez le hub : un hub non alimenté prive la carte de courant. |
| **Le menu des tables n'apparaît pas** | Ouvrez le panneau : si la ligne de l'adresse n'a pas la partie « Pont … », le pont ne parle pas à la carte. Le pont doit être allumé : `ponte.ps1 stato` (« Ponte ACCESO » et « Scheda: COMx »). Si la carte apparaît comme « non collegata » (non connectée), vérifiez le câble. Lancez `ponte.ps1 controllo` et lisez le rapport. Pendant une partie il n'y a pas de menu : il apparaît à la fin. Quand le pont est sur le câble, la carte lui redemande son salut toutes les 3 secondes : en rouvrant l'application, le menu revient tout seul. |
| **« Popper : base de données introuvable »** dans le panneau | Le pont ne trouve pas la base de Popper : vérifiez le dossier de Popper avec `ponte.ps1 controllo` (s'il est dans un dossier non standard, lancez le pont avec `--popper`). |
| **« Aucune table dans cette liste. »** | Normal pour **Récentes** ou **Les plus jouées** si vous n'avez pas encore joué ; essayez **Toutes**. |
| **« Le PC n'a pas envoyé la liste des tables : vérifiez Popper. »** | `ponte.ps1 controllo` : le dossier et la base de Popper ont-ils été trouvés ? |
| **Images manquantes (wheel, backglass)** | La première fois, les images arrivent du PC une par une ; avec une microSD, menu au repos, la carte les télécharge seule et ensuite tout est instantané. Le fichier dans Popper doit porter le nom du jeu (ou « Nom(1).png », « Nom (2).png »). Une image ajoutée dans Popper apparaît dans les 10 minutes, ou en rouvrant l'application. |
| **« Le câble vers le PC est occupé : réessayez dans un instant. »** | Le câble transférait une grande image : réessayez. |
| **« Les instructions ne sont pas arrivées du PC : réessayez. »** | La page s'est perdue sur le câble même après deux nouvelles demandes : réessayez dans un instant. |
| **« Disponible en fin de partie : pendant le jeu, le câble sert au DMD. »** | Normal pendant une table VPX : la fonction revient en fin de partie. |
| **« … s'est fermé tout seul : vous pouvez le relancer d'ici. »** | La table a planté ou a été fermée de force : touchez **Jouer** pour la relancer. |
| **« Le PC n'a pas répondu : impossible de décrire le fichier du backglass. »** | Le pont n'a pas répondu pour la Composition : vérifiez qu'il est allumé (`ponte.ps1 stato`) et rouvrez la Composition. |
| **« Pas d'instructions pour cette table. »** | Le PC n'a pas de carte d'instructions à envoyer pour cette table : ajoutez-en une dans Popper (écran GameHelp). |
| **La table ne démarre pas** (« La table n'a pas démarré : vérifiez le Web Remote de Popper. » / « Le PC n'a pas lancé la table : vérifiez Popper. ») | Popper doit tourner avec le Web Remote activé (`useWEB = true ;`, Popper redémarré, composant COM enregistré, port 80). Essayez de lancer la même table depuis Popper : si elle ne démarre pas non plus, le problème vient de la table ou de Popper. |
| **Impossible de quitter la partie depuis la carte** (« Le PC ne répond pas en Wi-Fi : quittez la partie sur la borne. » / « Le PC n'a pas fermé la partie : quittez-la sur la borne. ») | Pour les tables VPX, la sortie passe par le Wi-Fi : la carte doit être sur le réseau de la borne et joindre le Web Remote de Popper (TCP 80) sur le PC. En général il est joignable tel qu'installé ; sinon, dans le Pare-feu Windows Defender, autorisez les connexions entrantes TCP 80 pour PinUP Popper sur le réseau privé. En attendant, quittez avec les boutons de la borne. |
| **Quelle URL mettre pour le Wi-Fi ?** | Celle écrite sur la page d'attente de l'application ou en haut du panneau : `ws://<IP de la carte>:9090/dmd`. Si l'IP change à chaque démarrage, réservez une IP fixe dans la box. « Wi-Fi non connecté » : reliez la carte au réseau de la borne, ou utilisez l'USB. |
| **« Port : un nombre de 1 à 65535. » / « Impossible d'ouvrir le port … »** | Le port saisi n'est pas valable, ou la carte ne peut pas l'ouvrir : choisissez-en un autre dans **Port réseau**. Tant que ça ne marche pas, l'ancien port reste en service. |
| **Tables alphanumériques ou à segments (années 80) sans DMD sur la carte** | dmdext convertit seul les afficheurs à segments, mais seulement si VPinMAME lui transmet l'afficheur : pour cette ROM la clé de registre `HKCU\Software\Freeware\Visual PinMame\<rom>\showpindmd` doit valoir 1. Sauvegardez le registre avant de la modifier. |
| **« Ce DMD (…) ne tient pas sur l'écran. »** | En Wi-Fi le DMD dépasse 512x256 : utilisez une taille plus petite dans dmdext, ou l'USB. |
| **Image à l'envers ou couleurs bizarres** | **Écran de test** : le bloc plein doit être en haut à gauche (sinon **Rotation**) ; pour les DMD monochromes, choisissez la **Couleur**. |
| **Points petits ou « trop de noir » entre les lignes** | Utilisez **DMD plein écran** ou **DMD classique** au lieu de **DMD étiré** ; vérifiez avec la loupe **Vrais points, 1:1**. |
| **Le backglass n'apparaît pas alors que je l'ai choisi** | Avec l'écran pivoté, les dispositions avec images affichent le DMD classique : remettez la **Rotation** à 0°. |
| **Le panneau ne s'ouvre pas pendant la partie** | Gardez le doigt immobile sur l'écran 2 secondes entières (un toucher bref ouvre les actions rapides), ou appuyez sur Retour. |
| **Après un plantage de VPX, Popper ne lance plus rien** | Le pont ferme seul le serveur B2S resté ouvert (« closed b2sbackglassserverexe » dans le journal). Sinon, vérifiez que le pont est allumé. |

Pour signaler un problème, joignez la sortie de `ponte.ps1 stato` et les dernières lignes de
`%LOCALAPPDATA%\nucleo_bridge\ponte.log`.

---

## 10. Mettre à jour le firmware en toute sécurité

Les réglages de DMD Flipper sont dans la mémoire interne de la carte et survivent aux mises à jour.

- **Par Wi-Fi** (la méthode normale) : **Réglages → Mise à jour système**. Rien de particulier à faire pour le
  DMD.
- **Par le câble USB avec esptool** : tant que DMD Flipper est ouverte, la carte **ignore la réinitialisation par
  le port USB** (c'est ce qui la protège de dmdext), donc esptool ne pourrait pas la redémarrer. Avant d'écrire :
  1. fermez l'application : revenez à l'Accueil et répondez **Fermer** à « Fermer le DMD ? », ou appelez
     `GET /api/ui/home`, qui la ferme sans rien demander ;
  2. sur le PC, arrêtez le pont, qui occupe le port COM : `ponte.ps1 ferma` ;
  3. ne lancez pas de table pendant l'écriture ;
  4. une fois terminé : `ponte.ps1 avvia` et rouvrez DMD Flipper (ou laissez faire **Lancer au démarrage**).

---

## 11. Limites connues

- En USB la carte est un DMD 128x32 : dmdext adapte les autres tailles. En Wi-Fi elles sont affichées telles
  quelles, jusqu'à 512x256.
- En Wi-Fi, un seul PC à la fois.
- Avec **DMD plein écran**, le DMD se met à jour à environ 30 images par seconde ; avec les dispositions à points
  carrés, environ 52-60.
- D'où vient le DMD : les tables VPX l'envoient avec dmdext par USB ; les tables Future Pinball qui ont un DMD
  sont recopiées par le pont avec le mode mirror de dmdext ; Pinball FX, MAME et les jeux PC affichent les images
  de la vitrine du jeu.
- Sur le câble, les gros messages (images) arrivent parfois abîmés : le pont les renvoie jusqu'à ce que la carte
  les confirme ; rarement, une image peut manquer jusqu'au passage suivant.
- Sans microSD, les images passent par le câble à chaque fois.
- Chaque liste contient au plus 2000 tables.
- Avec l'écran pivoté, les dispositions avec images affichent le DMD classique.
- Les flyers et les instructions ne figurent pas dans la vitrine ; les tables masquées dans Popper n'apparaissent
  pas dans le menu.
- Les changements du moniteur du PC s'appliquent à la partie suivante (ou avec **Relancer la table**).
- Le Web Remote de Popper ne fonctionne que lorsque Popper tourne.

---

## 12. Confidentialité et réseau

- La carte ne parle **qu'au PC de la borne, sur le réseau local** : elle reçoit le DMD (USB, ou Wi-Fi sur le
  port 9090) et, pour quitter ou lancer une table pendant une partie VPX, elle appelle le Web Remote de Popper
  sur le PC (port 80). L'application n'envoie rien sur Internet.
- **Le serveur Wi-Fi du DMD (port 9090) n'a pas d'authentification** et **le Web Remote de Popper n'a pas de mot
  de passe** : utilisez-les uniquement sur un réseau domestique de confiance, jamais sur un réseau public et
  jamais exposés sur Internet.
- Le pont n'écoute que sur le PC lui-même (`127.0.0.1:58090`), lit la base de Popper en lecture seule et garde
  ses images converties dans `%LOCALAPPDATA%\nucleo_bridge\cache`. Il n'écrit dans les fichiers de B2S et de
  dmdext que lorsque vous choisissez une composition du moniteur (chapitre 8), avec sauvegarde.
- La carte garde les images reçues et les dernières listes sur la microSD, dans `/sdcard/data/dmd`.
