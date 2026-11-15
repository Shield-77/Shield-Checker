# Shield Checker

**Codex was used to generate the code**

Shield Checker aide à comprendre l’état d’une console 3DS avec des explications simples. Il lit des informations locales et peut comparer certaines versions à des releases publiques. **Il ne répare rien, ne télécharge aucun CIA et ne modifie aucun fichier système.** Les résultats sont informatifs : une absence ou un statut indisponible ne prouve pas une panne.

La version actuelle est [v1.1.0](https://github.com/Shield-77/Shield-Checker/releases/tag/v1.1.0), avec le CIA [ShieldChecker.cia](https://github.com/Shield-77/Shield-Checker/releases/download/v1.1.0/ShieldChecker.cia). Title ID : `000400000A210600`.

## Utiliser l’application

- **Vérification rapide** : compare automatiquement les versions de Luma3DS, GodMode9, Universal Updater, Shield Updater et hShop/3hs, puis affiche les points logiciels. Les résultats sont regroupés sans devoir ouvrir chaque outil.
- **Vérification avancée** : commence par les mêmes versions, puis ajoute modèle, mémoire accessible aux applications, batterie, carte SD, volume, casque, Wi-Fi et curseur 3D selon le modèle. Le test HTTPS avec R est facultatif.
- **Aide au diagnostic** : propose huit symptômes avec des observations et causes possibles, sans verdict matériel certain.

**Haut/Bas** navigue, **A** ouvre, **B** revient ou annule une requête, **START** quitte. Depuis les résultats des versions, **Y** ouvre les observations diagnostiques et **X** relance la comparaison en ligne. **L** revient aux versions. Dans le diagnostic avancé, **X** lance le test des commandes, **Y** le test des couleurs et **R** le test HTTPS facultatif. Dans Réglages, **A** change le niveau de détail et **X** la langue. La confirmation de langue apparaît au premier lancement ; maintenir X au démarrage la rouvre. Seul ce choix est enregistré dans `sdmc:/3ds/ShieldChecker/ui-language.dat`.

Les températures CPU et batterie ne sont pas mesurées. Le niveau de batterie est un palier de 0 à 5, pas un pourcentage d’usure. La mémoire affichée correspond à la région accessible aux applications. La présence d’un fichier ou d’un outil dans les chemins testés ne suffit pas à juger l’état de la console. Si le réseau échoue, les informations locales restent consultables. Le nouveau parcours de v1.1.0 a été compilé localement ; son test sur une Old 3DS réelle reste à faire.

## Source et licence

Le [code de v1.1.0](source/) et ses ressources de construction sont consultables directement dans le dépôt. L’[archive source exacte de la release](https://github.com/Shield-77/Shield-Checker/releases/download/v1.1.0/Shield-Checker-source-v1.1.0.zip) est conservée séparément. Les anciennes archives v1.0.3 et v1.1.0-dev restent accessibles dans l’historique Git, mais ne décrivent pas la release actuelle.

Le code du projet est publié sous [GPL-3.0-only](LICENSE). La police Atkinson Hyperlegible conserve sa [licence SIL OFL](assets/LICENSE-AtkinsonHyperlegible.txt). Les outils de construction ont leurs propres licences et sont à obtenir séparément ; voir [LICENSES/BUILD-TOOLS.md](LICENSES/BUILD-TOOLS.md). L’identité visuelle SHIELD ne doit pas servir à présenter un fork comme une release officielle.

## Compiler

Installer devkitPro/devkitARM, libctru, libcurl, Jansson et mbedTLS pour 3DS, ainsi que `makerom` et `bannertool`. Placer ces deux derniers aux chemins indiqués dans le Makefile, puis lancer `make`. Lancer `make -j1`. Les sorties prévues sont `build/ShieldChecker.elf` et `build/ShieldChecker.cia` (version CIA interne 7). Ce dépôt n’inclut pas les exécutables `makerom` et `bannertool`.
