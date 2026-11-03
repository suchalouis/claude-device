# Claude Pad — M5Stack Core2

Petit pad posé à côté du clavier :
- affiche ce que fait Claude Code (réflexion, outil en cours, terminé) ;
- **vibre et bipe** quand Claude attend ta réponse (rappel toutes les 30 s) ;
- **répond depuis l'écran tactile** aux demandes de Claude : permission
  (Oui / Oui, ne plus demander / Non), question à choix (simple ou multiple),
  validation de plan ;
- les zones tactiles **A / B / C** sous l'écran ouvrent trois vues :
  **Sessions** (toutes les sessions en cours, leur état et leur dernière
  activité ; toucher une ligne ouvre sa conso), **Conso** (contexte occupé,
  tokens, prix équivalent API) et **Direct** (l'état de Claude). Appuyer deux
  fois sur la même zone, ou attendre 30 s, ramène au direct.

Le dépôt a deux parties :
- **`server/`** : le firmware du pad (M5Stack Core2), à flasher une fois ;
- **`client/`** : les hooks Claude Code, à installer sur chaque PC dont tu
  veux suivre les sessions (macOS, Linux, Windows).

## Architecture

Rendu hors GitHub : ouvre [docs/architecture.html](docs/architecture.html) dans un navigateur.
Après avoir modifié un diagramme ci-dessous : `python3 docs/build-architecture.py`.

```mermaid
flowchart LR
  subgraph PC["Mac / Linux / Windows"]
    CC["Claude Code<br/>terminal · app · IDE<br/>(une ou plusieurs sessions)"]
    T[("transcript .jsonl")]
    subgraph H["client/ : hooks (~/.claude/settings.json)"]
      E["pad-event.py<br/>tous les événements"]
      P["pad-prompt.py<br/>PermissionRequest,<br/>AskUserQuestion, ExitPlanMode"]
      U["pad-usage.py<br/>PostToolUse, Stop<br/>(en arrière-plan)"]
    end
    CC --> E & P & U
    CC -.écrit.-> T
    T -.lu par.-> U
  end

  subgraph PAD["server/ : M5Stack Core2 (Wi-Fi, port 80)"]
    W["serveur HTTP"]
    S["sessions<br/>(état, conso)"]
    V["écran<br/>Direct · Sessions · Conso"]
    TS["tactile<br/>menu à l'écran<br/>zones A / B / C"]
    W --> S --> V
    TS --> V
  end

  E -- "POST /event" --> W
  P -- "POST /prompt" --> W
  P -- "GET /answer (attente)" --> W
  U -- "POST /usage" --> W
  P -. "décision / réponses" .-> CC
```

Une demande de Claude, de bout en bout :

```mermaid
sequenceDiagram
  participant CC as Claude Code
  participant P as pad-prompt.py
  participant Pad as Core2
  actor Toi
  CC->>P: hook (JSON : outil, question, plan...)
  P->>Pad: POST /prompt?id=...
  Pad-->>Toi: vibre, bipe, affiche les choix
  loop toutes les 0,3 s (90 s max)
    P->>Pad: GET /answer?id=...
    Pad-->>P: 202 en attente
  end
  Toi->>Pad: touche un choix
  P->>Pad: GET /answer?id=...
  Pad-->>P: 200 {"decision": "allow", "answers": [...]}
  P-->>CC: allow / deny / réponses
  Note over P,CC: sans réponse : POST /cancel,<br/>le menu du PC reprend la main
```

Le script de hook `pad-prompt.py` attend le choix fait sur le pad et le
renvoie directement à Claude Code (décision `allow` / `deny`, règle
« ne plus demander », réponses aux questions). Pas de clavier simulé :
ça marche dans le terminal comme dans l'app de bureau, quelle que soit la
fenêtre au premier plan. Pad éteint, délai dépassé (90 s) ou réponse donnée
sur le PC : le menu habituel de Claude Code reprend la main.

## 1. Flasher le pad (`server/`)

```bash
cd server
pio run -t upload
pio device monitor        # facultatif : affiche l'IP et http://claude-pad.local
```

Le Core2 utilise un pont USB CH9102 (modèles récents) ou CP2104 (anciens).
Si le port n'apparaît pas, installe le pilote correspondant.

### Régler le Wi-Fi

Au premier démarrage, le pad ouvre le point d'accès **Claude-Pad** et affiche
un QR code :
1. sur ton téléphone, rejoins le Wi-Fi « Claude-Pad » (ou scanne le code) ;
2. la page de réglage s'ouvre seule (sinon : http://192.168.4.1) ;
3. choisis ton réseau (2,4 GHz), tape son mot de passe : le pad redémarre
   et affiche son adresse.

Le réseau est gardé en mémoire, même après un nouveau flash. Pour en
changer : **touche l'écran pendant « Connexion à … »** au démarrage. Si le
réseau enregistré ne répond pas (box en panne), le pad rouvre le point
d'accès et réessaie toutes les 5 minutes.

Facultatif : `server/include/secrets.h` (copie de `secrets.h.example`) donne
un réseau par défaut tant qu'aucun n'a été choisi sur le pad, et permet de
changer le nom mDNS.

## 2. Brancher Claude Code (`client/`)

Il faut Python 3 sur la machine (déjà là sur macOS et Linux ; sous Windows,
python.org ou Microsoft Store) et le même réseau que le pad. Puis, sur
chaque machine :

```bash
python3 client/install.py 192.168.1.169   # macOS / Linux (ou ./client/install.sh)
py client\install.py 192.168.1.169        # Windows
python3 client/install.py --uninstall      # pour les retirer
```

Sans adresse, le script prend `http://claude-pad.local`. Sous Windows, donne
plutôt l'IP : la résolution `.local` n'y est pas toujours fiable.

Le script ajoute les hooks du pad dans `~/.claude/settings.json` sans toucher
au reste (sauvegarde en `.bak`) ; le relancer remplace l'adresse, et il
remplace aussi les anciens hooks `curl`. Il écrit le chemin exact de Python
et des scripts, avec l'adresse en argument (`--pad`) : pas de `curl` ni de
variable d'environnement dans les commandes, elles marchent quel que soit le
shell (bash, zsh, cmd, PowerShell, Git Bash). À la main : fusionne
`client/settings.json` en adaptant les chemins et l'adresse.

Les hooks :
- `pad-event.py` (tous les événements) transmet l'activité pour l'affichage.
  Il abandonne au bout de 1 s : Claude n'est jamais bloqué si le pad est éteint.
- `pad-prompt.py` (`PermissionRequest`, et `PreToolUse` pour
  `AskUserQuestion|ExitPlanMode`) attend le choix fait sur le pad, avec un
  `timeout` de 120 s (le script rend la main à 90 s, `CLAUDE_PAD_TIMEOUT`
  pour changer).
- `pad-usage.py` (`PostToolUse`, `Stop`) relit le transcript de la session et
  envoie au pad le contexte occupé, les tokens cumulés (sous-agents compris)
  et le prix au tarif de l'API Anthropic. Il se relance en arrière-plan et
  rend la main tout de suite.
- `SessionEnd` retire la session de la liste.

Test sans Claude :
```bash
curl -X POST http://claude-pad.local/event -H 'Content-Type: application/json' \
  -d '{"hook_event_name":"Notification","message":"Claude veut lancer: rm -rf build","cwd":"/home/louis/mon-projet"}'
```

> **Linux** : la résolution `.local` passe par avahi (`nss-mdns`). Si elle ne
> marche pas, remplace `claude-pad.local` par l'IP affichée au démarrage
> (et fixe-la par bail DHCP sur ta box).

## Limites (et suites possibles)

- Pendant que le pad attend, une **question** ou un **plan** n'apparaît pas
  sur le PC (le hook passe avant) : réponds sur le pad, ou attends le délai.
  Pour les **permissions** dans l'app de bureau, le menu s'affiche en même
  temps et la première réponse l'emporte.
- Plus de bouton STOP : un hook ne peut pas interrompre Claude. Refuser une
  demande depuis le pad arrête l'action en cours.
- Une seule demande à la fois pour toutes les sessions : la dernière gagne
  (la précédente retombe sur le menu du PC).
- Le prix est un **équivalent API** (tarifs codés dans `pad-usage.py`) : avec
  un abonnement Pro / Max, ce n'est pas ce que tu paies. Le contexte suppose
  une fenêtre de 1 M tokens (200 k pour Haiku 4.5).
- Le pad oublie les sessions au redémarrage, et celles sans nouvelles depuis 12 h.
- Questions : 4 au maximum, 4 options chacune (la réponse libre « Other »
  n'est pas proposée sur le pad).
- Pas d'authentification sur `/event`, `/prompt`, `/answer` : à garder sur un
  réseau local de confiance. Le point d'accès de réglage est ouvert (sans mot
  de passe) le temps du réglage.
- Le pad ne rejoint que les réseaux 2,4 GHz (limite de l'ESP32).
