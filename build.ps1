param([ValidateSet('x64','x86')][string]$Architecture='x64')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$env:GOCACHE=Join-Path $root '.cache/go-build'
$env:GOTOOLCHAIN='local'
$go=Get-Command go.exe -ErrorAction SilentlyContinue
if($go){$go=$go.Source}else{throw 'Go 1.25以降が必要です'}
# Prefer an already installed newer toolchain when the launcher is old.
$installed=Get-ChildItem (Join-Path $env:USERPROFILE 'go/pkg/mod/golang.org') -Directory -Filter 'toolchain@*.windows-amd64' -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
if($installed){$go=Join-Path $installed.FullName 'bin/go.exe'}
$env:GOOS='windows';$env:GOARCH='amd64';if($Architecture -eq 'x86'){$env:GOARCH='386'}
$dist=Join-Path $root "dist/$Architecture"
New-Item -ItemType Directory $dist -Force|Out-Null
Push-Location $root
try {
 & $go build -trimpath -o "$dist/cechctl.exe" ./cmd/cechctl
 if($LASTEXITCODE){throw 'cechctl build failed'}
 & $go build -trimpath -o "$dist/cech-host.exe" ./cmd/cech-host
 if($LASTEXITCODE){throw 'host build failed'}
 $vswhere="${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
 $vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
 if(!$vs){throw 'Visual Studio C++ tools are required'}
 $vcvars=Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
 $batch=Join-Path $dist 'compile.cmd'
 $source=Join-Path $root 'bon/BonDriver_CECH.cpp'
 $def=Join-Path $root 'bon/BonDriver_CECH.def'
 # LINK replaces import libraries; remove only these reproducible outputs first.
 foreach($name in @('BonDriver_CECH.lib','WinSCard.lib')) { $artifact=Join-Path $dist $name;if(Test-Path -LiteralPath $artifact){Remove-Item -LiteralPath $artifact} }
 # All paths originate from this script or vswhere, not user command text.
 @"
@call "$vcvars" $Architecture >nul
@if errorlevel 1 exit /b 1
@cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /DCECH_BON_BUILD /LD "$source" /Fo"$dist/BonDriver_CECH.obj" /link /DEF:"$def" /OUT:"$dist/BonDriver_CECH.dll" /IMPLIB:"$dist/BonDriver_CECH.lib"
@if errorlevel 1 exit /b 1
@cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /DWINSCARD_EXPORTS /DNOMINMAX /LD "$root/winscard/winscard.cpp" "$root/winscard/management.cpp" "$root/winscard/native_winscard.cpp" "$root/winscard/card_client.cpp" /Fo"$dist/" /link advapi32.lib /DEF:"$root/winscard/exports.def" /OUT:"$dist/WinSCard.dll" /IMPLIB:"$dist/WinSCard.lib"
@if errorlevel 1 exit /b 1
@cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE "$root/tests/bon_smoke.cpp" /Fo"$dist/bon_smoke.obj" /link /OUT:"$dist/bon_smoke.exe"
@if errorlevel 1 exit /b 1
@cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE "$root/tests/card_smoke.cpp" /Fo"$dist/card_smoke.obj" /link /OUT:"$dist/card_smoke.exe"
@if errorlevel 1 exit /b 1
"@ | Set-Content -LiteralPath $batch -Encoding ascii
 & cmd.exe /d /c $batch
 if($LASTEXITCODE){throw 'BonDriver build failed'}
 Copy-Item -LiteralPath "$root/README.md","$root/COPYING","$root/NOTICE" -Destination $dist
 Write-Host "Built: $dist"
}finally{Pop-Location;Remove-Item Env:GOOS,Env:GOARCH -ErrorAction SilentlyContinue}
