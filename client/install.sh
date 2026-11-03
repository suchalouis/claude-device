#!/bin/sh
# Branche Claude Code sur le Claude Pad (macOS / Linux). Demande jq.
#   ./install.sh [adresse du pad]     défaut : http://claude-pad.local
#   ./install.sh --uninstall          retire les hooks du pad
# CLAUDE_SETTINGS : autre fichier que ~/.claude/settings.json.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SETTINGS=${CLAUDE_SETTINGS:-$HOME/.claude/settings.json}
command -v jq >/dev/null || { echo "jq est requis (brew install jq, apt install jq)."; exit 1; }

PAD=
if [ "$1" != "--uninstall" ]; then
  PAD=${1:-http://claude-pad.local}
  PAD=${PAD%/}
  case $PAD in http://*|https://*) ;; *) PAD=http://$PAD ;; esac
  curl -s -m 3 -o /dev/null "$PAD/" && echo "Pad joignable : $PAD" ||
    echo "Attention : pas de réponse de $PAD (pad éteint ?). Les hooks sont installés quand même."
fi

mkdir -p "$(dirname "$SETTINGS")"
[ -f "$SETTINGS" ] && cp "$SETTINGS" "$SETTINGS.bak" || echo '{}' > "$SETTINGS"

# Retire les hooks du pad (y compris les anciens : scripts pad-*.py, curl .../event),
# puis ajoute ceux du modèle client/settings.json avec l'adresse du pad.
jq --arg pad "$PAD" --slurpfile t "$HERE/settings.json" '
  def ours: (.url // "" | test("/(event|hook)$"))
         or (.command // "" | test("pad-(event|prompt|usage)|/(event|usage)"));
  def clean: (.hooks // {}) | with_entries(
      .value |= (map(.hooks |= map(select(ours | not))) | map(select(.hooks | length > 0)))
      | select(.value | length > 0));
  .hooks = clean
  | if (.statusLine.command // "" | test("/usage")) then del(.statusLine) else . end
  | if $pad == "" then (if (.hooks | length) == 0 then del(.hooks) else . end) else
      reduce ($t[0].hooks | to_entries[]) as $e (.; .hooks[$e.key] += $e.value)
      | if .statusLine then . else .statusLine = $t[0].statusLine end
      | (.. | strings) |= sub("http://claude-pad.local"; $pad)
    end' "$SETTINGS" > "$SETTINGS.tmp" && mv "$SETTINGS.tmp" "$SETTINGS"

if [ -n "$PAD" ] && ! jq -e '.statusLine.command | test("/usage")' "$SETTINGS" >/dev/null; then
  echo "Une ligne de statut existe déjà : gardée telle quelle, la vue Conso restera vide (voir README)."
fi
[ -n "$PAD" ] && ACTION="installés dans" || ACTION="retirés de"
echo "Hooks du pad $ACTION $SETTINGS (sauvegarde : $SETTINGS.bak)."
