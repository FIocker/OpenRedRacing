# third_party

Du code qui n'est pas le nôtre, gardé **dans le dépôt**.

⚠️ **C'est l'exception, pas la règle.** SDL2, RmlUi et ENet restent *hors du dépôt* (le
`CMakeLists.txt` racine les télécharge, ou prend le dossier qu'on lui donne) parce que ce sont
des arbres de sources entiers. Un fichier unique du domaine public ne justifie pas ce détour : pointer CMake vers un
chemin externe coûterait plus cher que de le poser ici, et le jour où la machine change, le
projet compile encore.

| Fichier | Version | Licence | Pourquoi |
|---|---|---|---|
| `stb_vorbis.c` | 1.22 (2021) | domaine public / MIT au choix | Décode les `.ogg` de `MUSIC/`. SDL2 seul ne décode rien ; `SDL2_mixer` serait une dépendance bien plus lourde pour un seul format |
| `lodepng.h`, `lodepng.cpp` | 20200306 | zlib | Décode (et réencode) le **logo du joueur**, un PNG ([`../ui/team_logos.h`](../ui/team_logos.h)). Copié tel quel depuis les dépendances de test de RmlUi 6.3 (`Tests/Dependencies/lodepng/`), déjà sur la machine. Compilé comme une source ordinaire (`OPENBIGRED_APP_SOURCES`) |

`stb_vorbis.c` est **compilé une seule fois**, inclus directement par
[`../audio/music.cpp`](../audio/music.cpp) — c'est la façon dont il est prévu d'être utilisé.
Il compile proprement en C++20 avec la chaîne MinGW du projet, sans modification : **ne pas le
retoucher**, pour qu'une mise à jour reste un simple remplacement de fichier.
