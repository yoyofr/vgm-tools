# Rewamp : passage au format VGM v3 (ES55xx, ZSG-2, Namco WSG)

Instructions pour mettre à jour le lecteur VGM de rewamp. Référence du format :
`VGM-proposal-v3-allocation.md` dans https://github.com/yoyofr/vgm-tools.

## 1. Contexte

- Les fichiers ES5505/ES5506/ES5510 (Taito F3, Seta SSV…) et ZSG-2/TMS57002 (Taito Zoom) existent en deux
  encodages : **v1** (packs publiés jusqu'au 31/10/2026, lus par la branche `es55xx` de libvgm) et
  **v3** (captures MAME à partir du 31/10/2026, packs reconvertis, lus par la branche `vgm-v3`).
- Les deux annoncent la version VGM **1.72** : le numéro de version ne permet pas de les distinguer.
- La v3 ajoute les chips Namco WSG / C15 / C30 / Pole Position WSG (libvgm PR #151), qui occupent les
  identifiants et les offsets d'en-tête que la v1 utilisait pour le ZSG-2.
- Un lecteur v3 ne joue pas correctement un fichier v1, et inversement. Rewamp doit embarquer le lecteur
  v3 et convertir les fichiers v1 au chargement.

## 2. libvgm

Passer de la branche `es55xx` à la branche `vgm-v3` de https://github.com/yoyofr/libvgm (commit `d892a5c`,
= `es55xx` + PR #151 de cam900 + commandes v3).

Fichiers ajoutés, à intégrer au build de rewamp s'il a sa propre liste de sources :

- `emu/cores/namcowsg.c/.h`, `namcoc15.c/.h`, `namcoc30.c/.h`, `namcoppw.c/.h`
- défines `SNDDEV_NAMCOWSG`, `SNDDEV_NAMCOC15`, `SNDDEV_NAMCOC30`, `SNDDEV_NAMCOPPW` (cf. `emu/CMakeLists.txt`)

Fichiers modifiés : `emu/SoundDevs.h`, `emu/SoundEmu.c`, `emu/dac_control.c`, `emu/cores/zsg2.c/.h`,
`player/vgmplayer.cpp/.hpp`, `player/vgmplayer_cmdhandler.cpp`.

**Identifiants de chips changés** (`emu/SoundDevs.h`) : tout code de rewamp qui dépend des DEVID (noms de
chips, voix, mute, oscilloscopes, hooks Modizer…) doit être revu.

| DEVID | Chip |
|---|---|
| 0x30 | Namco WSG (nouveau) |
| 0x31 | Namco C15 (nouveau) |
| 0x32 | Namco C30 (nouveau ; bit 31 de l'horloge = stéréo) |
| 0x33 | Namco Pole Position WSG (nouveau, 4 sorties) |
| 0x34 | ZSG-2 (était 0x30) |

## 3. Conversion des fichiers v1 au chargement

Prendre `vgm_v3.h` dans https://github.com/yoyofr/vgm-tools (branche `main`). C'est un en-tête C++17 seul,
sans dépendance (std uniquement), et c'est la conversion utilisée pour reconvertir les packs, vérifiée
octet pour octet par rendu sur tous les packs.

Dans le chargement d'un VGM, **après décompression gzip** et avant de passer les données à libvgm :

```cpp
#include "vgm_v3.h"

// data : fichier VGM décompressé
static bool vgm_is_v1_ext(const std::vector<uint8_t>& d)
{
	if (d.size() < 0x40 || memcmp(d.data(), "Vgm ", 4)) return false;
	uint32_t ver = vgmv3::rd32(d, 0x08);
	size_t hdr = (ver >= 0x150 && vgmv3::rd32(d, 0x34)) ? 0x34 + vgmv3::rd32(d, 0x34) : 0x40;
	if (hdr >= 0x120) return false;                                       // v3 (ou autre format récent)
	bool es  = hdr >= 0xD4  && vgmv3::rd32(d, 0xD0)  != 0;                 // ES5505 / ES5506
	bool zsg = hdr == 0x110 && vgmv3::rd32(d, 0x100) != 0;                 // ZSG-2 v1
	return es || zsg;
}

if (vgm_is_v1_ext(data))
{
	try
	{
		int kind;
		data = vgmv3::to_v3(data, kind);   // kind : vgmv3::ES ou vgmv3::ZSG
	}
	catch (const std::exception& e)
	{
		// commande v1 sans équivalent v3 (jamais rencontré sur les packs publiés) :
		// signaler le fichier (e.what()) au lieu de le jouer faux
	}
}
```

Points importants :

- Ne toucher à rien d'autre : un fichier sans horloge ES5505/6 ni ZSG-2, ou un en-tête de 0x120 ou plus,
  est passé tel quel (ce test d'en-tête est celui des outils vgm-tools).
- La conversion se fait en mémoire et ne modifie pas le fichier sur le disque. Le point de boucle, le
  nombre d'échantillons et le tag GD3 sont conservés : durée, boucle et tags affichés ne changent pas.
- Si rewamp lit le GD3 ou l'en-tête lui-même, le faire sur les données **après** conversion, ou
  avant : les deux donnent le même résultat. En revanche, l'offset de données (0x34) et la taille du
  fichier changent.
- Les fichiers v1 « 1.73 » (captures d'avant le 23/10/2026) sont gérés : ils ressortent en 1.72.

## 4. Vérification

1. Un pack v1 (ex. Darius Gaiden, Taito F3) et le même pack converti en v3 (`v3pack.py` ou VGM Studio,
   onglet Conversion v3) doivent sonner identiques dans rewamp, piste par piste.
2. Un fichier ZSG-2 v1 (G-Darius, Ray Crisis, Super Puzzle Bobble) doit afficher et jouer un ZSG-2 et non
   un Namco WSG (symptôme si la conversion n'est pas faite : son de type Pac-Man, ou silence).
3. Une capture Namco v3 (Mappy, Rolling Thunder, Dragon Spirit, Galaga, Pole Position) doit jouer avec les
   nouveaux cores.
4. Un VGM de chip classique (YM2151, SegaPCM…) ne doit pas changer.

Référence : `vgm2wav` de la branche `vgm-v3` (`--loops 2 --fade 0`) donne le rendu attendu d'un fichier
v3 ; `vgm_v3conv in.vgz out.vgz` fait la même conversion en ligne de commande.

## 5. À savoir

- La v3 est une proposition : Valley Bell ne l'a pas encore intégrée à libvgm. Si les numéros changent à
  l'intégration, il y aura une nouvelle conversion du même type ; garder la conversion au chargement
  plutôt que de compter sur des fichiers tous à jour.
- `vgm_v3.h` sait aussi faire l'inverse (`vgmv3::to_v1`), inutile pour rewamp.
