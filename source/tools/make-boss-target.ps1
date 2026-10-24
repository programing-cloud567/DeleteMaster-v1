param(
    [string]$VhdPath = "$env:ProgramData\DeleteMaster\boss.vhdx",
    [string]$Letter = "V",
    [switch]$Remove,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Запусти от имени администратора.'
}

$target = "${Letter}:\DeleteMaster-BossTarget"
$root = "${Letter}:\"

function Test-VolumeWritable([string]$path) {
    $probe = Join-Path $path '__write_probe.tmp'
    try {
        [IO.File]::WriteAllText($probe, 'probe')
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        return $true
    } catch {
        return $false
    }
}

if ($Remove) {
    Write-Output 'снимаю стенд...'
    Dismount-DiskImage -ImagePath $VhdPath -ErrorAction SilentlyContinue | Out-Null
    Start-Sleep -Seconds 2
    if (Test-Path -LiteralPath $VhdPath) {
        try { Remove-Item -LiteralPath $VhdPath -Force -ErrorAction Stop }
        catch { Write-Output ('VHD не удалился: ' + $_.Exception.Message) }
    }
    Write-Output ('VHD на месте: ' + (Test-Path -LiteralPath $VhdPath))
    return
}

if (Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue) {
    throw "Буква $Letter занята. Запусти с -Letter W."
}

New-Item -ItemType Directory -Force -Path (Split-Path $VhdPath) | Out-Null
if (Test-Path -LiteralPath $VhdPath) {
    if (-not $Force) { throw "Файл уже существует: $VhdPath. Добавь -Force." }
    Dismount-DiskImage -ImagePath $VhdPath -ErrorAction SilentlyContinue | Out-Null
    Start-Sleep -Seconds 2
    Remove-Item -LiteralPath $VhdPath -Force
}

Write-Output 'создаю VHD...'
@"
create vdisk file="$VhdPath" maximum=96 type=expandable
select vdisk file="$VhdPath"
attach vdisk
create partition primary
format fs=ntfs quick label=DMBOSS
assign letter=$Letter
exit
"@ | diskpart | Out-String | Write-Output
Start-Sleep -Seconds 2

if (-not (Test-Path -LiteralPath $root)) { throw "Том $Letter`: не появился." }

Write-Output 'наполняю...'
New-Item -ItemType Directory -Force -Path $target, "$target\bin", "$target\core", "$target\data" | Out-Null

$engineDll = @(
    "$env:USERPROFILE\OneDrive\Desktop\engines\DeleteEngine.Verify.dll",
    "$env:USERPROFILE\Desktop\engines\DeleteEngine.Verify.dll"
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $engineDll) { throw 'Не нашёл образец DLL (Desktop\engines).' }

$n = 0
foreach ($dir in @("$target\bin", "$target\core", "$target\data")) {
    for ($i = 1; $i -le 60; $i++) {
        $n++
        Copy-Item -LiteralPath $engineDll -Destination (Join-Path $dir ("module_{0:d4}.dll" -f $n)) -Force
    }
}
for ($i = 1; $i -le 5; $i++) {
    Set-Content -LiteralPath (Join-Path $target ("hello{0}.exe" -f $i)) -Value ("MZ hello #$i") -Encoding ASCII
}
Set-Content -LiteralPath (Join-Path $target 'README.txt') -Value 'read-only VHD target' -Encoding UTF8

Write-Output 'ставлю ACL-запрет...'
cmd /c "icacls `"$target`" /deny *S-1-1-0:(RD,D) /T /C /Q" | Out-Null
cmd /c "attrib +S +H `"$target`"" | Out-Null

Write-Output 'отмонтирую...'
Dismount-DiskImage -ImagePath $VhdPath | Out-Null
Start-Sleep -Seconds 3

Write-Output 'монтирую ТОЛЬКО ДЛЯ ЧТЕНИЯ...'
Mount-DiskImage -ImagePath $VhdPath -Access ReadOnly | Out-Null
Start-Sleep -Seconds 4

$img = Get-DiskImage -ImagePath $VhdPath
if (-not $img.Attached) { throw 'VHD не присоединился.' }

if (-not (Test-Path -LiteralPath $root)) {
    Write-Output 'буква не восстановилась, назначаю заново...'
    @"
select vdisk file="$VhdPath"
select partition 1
assign letter=$Letter
exit
"@ | diskpart | Out-String | Write-Output
    Start-Sleep -Seconds 2
}

$writable = Test-VolumeWritable $root
if ($writable) {
    Write-Output ''
    Write-Output '!!! ОШИБКА: том ОСТАЛСЯ ДОСТУПЕН ДЛЯ ЗАПИСИ — это не стенд, а обычный диск.'
    Write-Output '!!! Никакого смысла тестировать в таком виде. Проверь редакцию Windows / права.'
    exit 2
}

$contentOk = Test-Path -LiteralPath $target
$count = 0
if ($contentOk) {
    $count = (Get-ChildItem -LiteralPath $target -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object).Count
}

Write-Output ''
Write-Output '=== СТЕНД ГОТОВ (read-only подтверждён) ==='
Write-Output ("Том: " + $Letter + ":, attached=" + $img.Attached + ", запись запрещена")
Write-Output ("Цель: " + $target)
Write-Output ("Файлов внутри: " + $count)
Write-Output 'VHD держится системой, поэтому и сам контейнер удалить нельзя, пока том присоединён.'
