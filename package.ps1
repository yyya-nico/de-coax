param([string]$Label='20261027')
$ErrorActionPreference='Stop'
if($Label -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Label must contain only letters, digits, underscores, or hyphens'}
$root=$PSScriptRoot
$common=@('README.md','COPYING','NOTICE','LICENSE-APACHE','docs','driver') | ForEach-Object {Join-Path $root $_}
foreach($architecture in @('x64','x86')){
 $folder=Join-Path $root "dist/$architecture"
 $files=@('BonDriver_CECH.dll','WinSCard.dll','cech-host.exe','cechctl.exe','bon_smoke.exe','card_smoke.exe') | ForEach-Object {Join-Path $folder $_}
 foreach($file in $files){if(!(Test-Path -LiteralPath $file)){throw "Build output is missing: $file"}}
 Compress-Archive -LiteralPath ($files+$common) -DestinationPath (Join-Path $root "dist/CECH-ZD1J-$Label-$architecture.zip") -Force
}
$sources=@('cmd','receiver','internal','pkg','bon','winscard','tests','build.ps1','package.ps1','go.mod','.gitignore','.clang-format') | ForEach-Object {Join-Path $root $_}
Compress-Archive -LiteralPath ($sources+$common) -DestinationPath (Join-Path $root "dist/CECH-ZD1J-$Label-source.zip") -Force
$hashes=foreach($architecture in @('x64','x86')){
 foreach($name in @('BonDriver_CECH.dll','WinSCard.dll','cech-host.exe','cechctl.exe')){
  $h=Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $root "dist/$architecture/$name")
  "$($h.Hash)  $architecture/$name"
 }
}
$hashes | Set-Content -LiteralPath (Join-Path $root 'dist/SHA256SUMS.txt')
Get-ChildItem -LiteralPath (Join-Path $root 'dist') -Filter "CECH-ZD1J-$Label-*.zip" | Select-Object Name,Length
