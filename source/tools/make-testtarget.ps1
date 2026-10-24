param(
    [string]$Root = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'DeleteMaster-TestTarget')
)

$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw 'vcvars64.bat не найден' }
if (Test-Path -LiteralPath $Root) { throw "Папка уже существует: $Root" }

$bin = Join-Path $Root 'bin'
$lib = Join-Path $Root 'lib'
$core = Join-Path $Root 'core'
$mods = Join-Path $Root 'modules'
$plugins = Join-Path $Root 'plugins'
$locked = Join-Path $Root 'locked'
$tmp = Join-Path $env:TEMP 'dmtarget-obj'

foreach ($d in @($Root, $bin, $lib, $core, $mods, $plugins, $locked, $tmp)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
for ($i = 1; $i -le 10; $i++) {
    New-Item -ItemType Directory -Force -Path (Join-Path $plugins ('p{0:d2}' -f $i)) | Out-Null
}

Write-Output 'готовлю локальные исходники...'
$localSrc = Join-Path $tmp 'src'
New-Item -ItemType Directory -Force -Path $localSrc | Out-Null
foreach ($f in @('hello.cpp', 'module.cpp', 'dmlocker.cpp', 'dmguard.cpp')) {
    Copy-Item -LiteralPath (Join-Path (Join-Path $src 'testtarget') $f) -Destination $localSrc -Force
}

Write-Output 'компилирую...'
$batLines = @('@echo off')
$batLines += 'call "' + $vcvars + '" >nul 2>&1'
for ($i = 1; $i -le 5; $i++) {
    $batLines += 'cl /nologo /EHsc /O2 /MD /DHELLO_ID=' + $i + ' /Fo"' + $tmp + '\hello' + $i + '.obj" /Fe"' +
                 $bin + '\hello' + $i + '.exe" "' + $localSrc + '\hello.cpp" >nul'
}
$batLines += 'cl /nologo /EHsc /O2 /MD /LD /Fo"' + $tmp + '\module.obj" /Fe"' + $lib + '\_template.dll" "' +
             $localSrc + '\module.cpp" >nul'
$batLines += 'cl /nologo /EHsc /O2 /MD /Fo"' + $tmp + '\dmlocker.obj" /Fe"' + $Root + '\dmlocker.exe" "' +
             $localSrc + '\dmlocker.cpp" >nul'
$batLines += 'cl /nologo /EHsc /O2 /MD /Fo"' + $tmp + '\dmguard.obj" /Fe"' + $Root + '\dmguard.exe" "' +
             $localSrc + '\dmguard.cpp" >nul'
$batPath = Join-Path $tmp 'build.bat'
[IO.File]::WriteAllLines($batPath, $batLines, [Text.Encoding]::Default)
cmd /c "`"$batPath`"" | Out-Null

for ($i = 1; $i -le 5; $i++) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin "hello$i.exe"))) { throw "не собрался hello$i.exe" }
}
$template = Join-Path $lib '_template.dll'
if (-not (Test-Path -LiteralPath $template)) { throw 'не собрался шаблон DLL' }
if (-not (Test-Path -LiteralPath (Join-Path $Root 'dmlocker.exe'))) { throw 'не собрался dmlocker.exe' }

Write-Output 'раскладываю 500 DLL...'
$n = 0
$plan = @(
    @{ Dir = $bin;      Count = 10;  Prefix = 'dm_bin_' },
    @{ Dir = $core;     Count = 80;  Prefix = 'dm_core_' },
    @{ Dir = $lib;      Count = 120; Prefix = 'dm_lib_' },
    @{ Dir = $mods;     Count = 140; Prefix = 'dm_module_' }
)
for ($i = 1; $i -le 10; $i++) {
    $plan += @{ Dir = (Join-Path $plugins ('p{0:d2}' -f $i)); Count = 15; Prefix = "dm_plugin_p{0:d2}_" -f $i }
}
foreach ($item in $plan) {
    for ($k = 1; $k -le $item.Count; $k++) {
        $n++
        $name = $item.Prefix + ('{0:d4}' -f $k) + '.dll'
        Copy-Item -LiteralPath $template -Destination (Join-Path $item.Dir $name) -Force
    }
}
Remove-Item -LiteralPath $template -Force

Write-Output 'создаю прочие файлы...'
$readme = @'
DeleteMaster Test Target
=======================

Тестовый полигон для проверки принудительного удаления.
Содержимое: 5 hello-world exe, 500 DLL, вложенные папки.

Защита:
 - ACL: Everyone запрещено DELETE и FILE_DELETE_CHILD (рекурсивно)
 - атрибуты System+Hidden на корне, ReadOnly на файлах
 - процесс dmlocker.exe держит корневую папку и файлы в locked\ без общего доступа

Если DeleteMaster не справился, убери защиту вручную:
  1) taskkill /IM dmlocker.exe /F
  2) takeown /F "ЭТА_ПАПКА" /R /D Y
  3) icacls "ЭТА_ПАПКА" /reset /T /C
  4) attrib -R -S -H "ЭТА_ПАПКА" /S /D
  5) rd /S /Q "ЭТА_ПАПКА"
'@
Set-Content -LiteralPath (Join-Path $Root 'README.txt') -Value $readme -Encoding UTF8

for ($i = 1; $i -le 6; $i++) {
    Set-Content -LiteralPath (Join-Path $locked ("locked_{0:d2}.dat" -f $i)) -Value ("locked payload $i" * 20) -Encoding UTF8
}
Set-Content -LiteralPath (Join-Path $Root 'build.info') -Value ("{0}`nmodules: 500`n" -f (Get-Date)) -Encoding UTF8

Write-Output 'включаю защиту: атрибуты...'
Get-ChildItem -LiteralPath $Root -Recurse -File -Force | ForEach-Object {
    try { $_.Attributes = $_.Attributes -bor [IO.FileAttributes]::ReadOnly } catch { }
}
$rootItem = Get-Item -LiteralPath $Root -Force
$rootItem.Attributes = $rootItem.Attributes -bor [IO.FileAttributes]::System -bor [IO.FileAttributes]::Hidden

Write-Output 'включаю защиту: запрет удаления на каждом файле...'
$sid = New-Object System.Security.Principal.SecurityIdentifier 'S-1-1-0'
$denyDelete = New-Object System.Security.AccessControl.FileSystemAccessRule(
    $sid,
    [System.Security.AccessControl.FileSystemRights]::Delete,
    [System.Security.AccessControl.AccessControlType]::Deny)
$denied = 0
Get-ChildItem -LiteralPath $Root -Recurse -File -Force | ForEach-Object {
    try {
        $acl = Get-Acl -LiteralPath $_.FullName
        $acl.AddAccessRule($denyDelete)
        Set-Acl -LiteralPath $_.FullName -AclObject $acl
        $denied++
    } catch { }
}
Write-Output "  deny DELETE на файлах: $denied"

$dllCount = (Get-ChildItem -LiteralPath $Root -Recurse -Filter *.dll -File -Force).Count
$exeCount = (Get-ChildItem -LiteralPath $Root -Recurse -Filter *.exe -File -Force).Count
$allCount = (Get-ChildItem -LiteralPath $Root -Recurse -File -Force).Count
$sizeMB = [math]::Round(((Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Measure-Object Length -Sum).Sum / 1MB), 1)

Write-Output 'включаю защиту: отказ в доступе на папках...'
$dirList = @($Root) + (Get-ChildItem -LiteralPath $Root -Directory -Force | Select-Object -ExpandProperty FullName)
foreach ($d in $dirList) {
    cmd /c "icacls `"$d`" /deny *S-1-1-0:(RD,D)" | Out-Null
}

Write-Output 'запускаю сеть держателей...'
Start-Process -FilePath (Join-Path $Root 'dmguard.exe') -ArgumentList @("`"$Root`"", 'guard', '3') -WindowStyle Hidden
Start-Process -FilePath (Join-Path $Root 'dmguard.exe') -ArgumentList @("`"$Root`"", 'watch', '1') -WindowStyle Hidden
Start-Sleep -Seconds 2

Write-Output ''
Write-Output "готово: $Root"
Write-Output "DLL: $dllCount, EXE: $exeCount, всего файлов: $allCount, объём: $sizeMB МБ"
