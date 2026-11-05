# Shield Checker pour console 3DS

Shield Checker aide a voir simplement si les outils importants de la console sont a jour. Il verifie Luma3DS, GodMode9, Universal Updater, Shield Updater et le client hShop/3hs s'ils sont installes sur la carte SD.

Cette application est encore en developpement. Ses controles ne couvrent pas tous les chemins ou configurations possibles : les resultats sont informatifs et ne remplacent pas un guide officiel.

## A savoir

Shield Checker est un outil de consultation seulement. Il ne telecharge ni n'installe d'application et ne modifie aucun fichier systeme. Il compare les versions locales avec les dernieres versions stables trouvees sur GitHub. La recherche en ligne se fait au lancement de la verification; parcourir les resultats ne relance pas le reseau. Il enregistre uniquement le choix de langue dans sdmc:/3ds/ShieldChecker/ui-language.dat.

L'interface propose le francais et l'anglais. Au premier lancement, elle suggere la langue detectee et demande confirmation. Maintiens X au demarrage pour revoir cette confirmation et changer la langue.

## Comprendre les resultats

- **VERIFIE ET A JOUR** : la version trouvee correspond a la derniere version stable consultee. Aucune action n'est necessaire.
- **NOUVELLE VERSION TROUVEE** : une version plus recente existe. Checker explique a quoi sert l'outil; il ne fait pas la mise a jour. Consulte le guide officiel avant d'agir.
- **FICHIER NON TROUVE SUR SD** : le payload GodMode9 n'a pas ete trouve dans les chemins controles. Il peut etre installe autrement ou ne pas etre utilise. Ce n'est pas une alerte.
- **STATUT INDISPONIBLE** : Checker n'a pas pu confirmer. Cela peut venir du Wi-Fi de la 3DS, de GitHub momentanement indisponible, d'un lien de release change ou d'un fichier/version qu'il ne sait pas lire. Rien n'est modifie; tu peux reessayer plus tard avec X.

Luma3DS est affiche en premier car il est important au fonctionnement des fonctions homebrew. Checker lit sa version active et indique separement si `boot.firm` est present sur la SD : Luma peut aussi demarrer depuis CTRNAND. Pour GodMode9, Checker examine les chemins de payload habituels et signale le dossier `gm9` comme facultatif. Les trois applications sont reperees dans la liste des titres installes sur la SD. Pour hShop/3hs, Checker affiche seulement la presence et la comparaison des versions, sans conseil de mise a jour.

## Commandes

- Accueil : **A** demarre la verification; **B** quitte.
- Resultats : **Haut/Bas** choisit un outil; **A** affiche une explication; **X** relance la verification; **START** quitte.
- Details : **B** revient aux resultats; **Haut/Bas** change d'outil.

## Taille du CIA

La musique d'ambiance est desactivee et ses cinq fichiers ne sont pas inclus dans le CIA ni dans l'archive source v1.0.3. Ces fichiers audio d'origine ne sont plus necessaires pour compiler cette version. Les verifications ne demandent plus l'acces au DSP.

## Identite de l'application

- Nom : Shield Checker
- Title ID : `000400000A210600`
- Icone : `icon.png`
- Banniere CIA : `banner.png`

## Compilation

Necessite devkitPro/devkitARM, libctru, libcurl, Jansson, mbedTLS, bannertool et makerom. Les executables tiers bannertool et makerom ne sont pas redistribues dans l'archive source ; il faut les obtenir separement depuis leurs projets et les placer dans `tools/`. Voir `LICENSES/BUILD-TOOLS.md`.

```sh
make
```

Sorties : `build/ShieldChecker.cia` et `build/ShieldChecker.elf`.

