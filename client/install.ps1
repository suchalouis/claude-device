# Branche Claude Code sur le Claude Pad (Windows ; PowerShell 5.1 ou plus).
#   .\install.ps1 [adresse du pad]     défaut : http://claude-pad.local
#   .\install.ps1 -Uninstall           retire les hooks du pad
# Si PowerShell refuse le script : powershell -ExecutionPolicy Bypass -File install.ps1 192.168.1.49
param([string]$Pad = "http://claude-pad.local", [switch]$Uninstall)
$ErrorActionPreference = "Stop"

$Pad = $Pad.TrimEnd("/")
if ($Pad -notmatch "^https?://") { $Pad = "http://$Pad" }
if (-not $Uninstall) {
  try { Invoke-WebRequest -Uri "$Pad/" -TimeoutSec 3 -UseBasicParsing | Out-Null; "Pad joignable : $Pad" }
  catch { if ($_.Exception.Response) { "Pad joignable : $Pad" } else { "Attention : pas de réponse de $Pad (pad éteint ?). Les hooks sont installés quand même." } }
}

$path = if ($env:CLAUDE_SETTINGS) { $env:CLAUDE_SETTINGS } else { Join-Path $HOME ".claude\settings.json" }
New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
$settings = [pscustomobject]@{}
if (Test-Path $path) {
  Copy-Item $path "$path.bak" -Force
  $raw = Get-Content $path -Raw -Encoding UTF8
  if ($raw.Trim()) { $settings = $raw | ConvertFrom-Json }
}

function Test-Ours($h) {
  ($h.url -match "/(event|hook)$") -or ($h.command -match "pad-(event|prompt|usage)|/(event|usage)")
}

# Retire les hooks du pad (y compris les anciens)
$hooks = [ordered]@{}
if ($settings.hooks) {
  foreach ($ev in $settings.hooks.PSObject.Properties) {
    $entries = @()
    foreach ($e in @($ev.Value)) {
      $kept = @(@($e.hooks) | Where-Object { -not (Test-Ours $_) })
      if ($kept.Count) { $e.hooks = $kept; $entries += $e }
    }
    if ($entries.Count) { $hooks[$ev.Name] = $entries }
  }
}
if ($settings.statusLine.command -match "/usage") { $settings.PSObject.Properties.Remove("statusLine") }

$noStatus = $false
if (-not $Uninstall) {
  # Modèle client\settings.json, avec l'adresse du pad (curl.exe : « curl » est
  # un alias de Invoke-WebRequest dans PowerShell 5)
  $tpl = (Get-Content (Join-Path $PSScriptRoot "settings.json") -Raw -Encoding UTF8) -replace "http://claude-pad\.local", $Pad
  $tpl = $tpl | ConvertFrom-Json
  foreach ($ev in $tpl.hooks.PSObject.Properties) {
    $hooks[$ev.Name] = @(@($hooks[$ev.Name]) | Where-Object { $_ }) + @($ev.Value)
  }
  if ($settings.statusLine) { $noStatus = $true }
  else {
    $tpl.statusLine.command = $tpl.statusLine.command -replace "^curl ", "curl.exe "
    $settings | Add-Member statusLine $tpl.statusLine -Force
  }
}
if ($hooks.Count) { $settings | Add-Member hooks ([pscustomobject]$hooks) -Force }
else { $settings.PSObject.Properties.Remove("hooks") }

$json = $settings | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($path, $json + "`n", (New-Object Text.UTF8Encoding $false))
if ($noStatus) { "Une ligne de statut existe déjà : gardée telle quelle, la vue Conso restera vide (voir README)." }
"Hooks du pad $(if ($Uninstall) { 'retirés de' } else { 'installés dans' }) $path (sauvegarde : $path.bak)."
