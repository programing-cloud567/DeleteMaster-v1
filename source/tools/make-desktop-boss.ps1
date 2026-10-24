param(
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Запусти от имени администратора.'
}

$desktop = [Environment]::GetFolderPath('Desktop')
$root = Join-Path $desktop 'DM-Boss'
$user = "$env:USERDOMAIN\$env:USERNAME"
$quote = [char]34

if ($Remove) {
    Write-Output 'снимаю защиту...'
    Get-Process dmguard,dmlocker -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
    cmd /c "takeown /F $quote$root$quote /R /D Y" | Out-Null
    cmd /c "icacls $quote$root$quote /reset /T /C /Q" | Out-Null
    cmd /c "icacls $quote$root$quote /grant ""${user}:(OI)(CI)F""" | Out-Null
    cmd /c "attrib -R -S -H $quote$root$quote /S /D" | Out-Null
    cmd /c "rd /S /Q $quote$root$quote" | Out-Null
    Write-Output ('удалено: ' + (-not (Test-Path -LiteralPath $root)))
    return
}

if (Test-Path -LiteralPath $root) {
    throw "Уже существует: $root. Сначала запусти с -Remove."
}

New-Item -ItemType Directory -Force -Path $root, "$root\bin", "$root\core", "$root\locked" | Out-Null

Write-Output 'наполняю...'
$sample = Join-Path $desktop 'engines\DeleteEngine.Verify.dll'
if (Test-Path -LiteralPath $sample) {
    $n = 0
    foreach ($dir in @("$root\bin", "$root\core")) {
        for ($i = 1; $i -le 100; $i++) {
            $n++
            Copy-Item -LiteralPath $sample -Destination (Join-Path $dir ("module_{0:d4}.dll" -f $n)) -Force
        }
    }
} else {
    Write-Output '  образец DLL не найден, только текстовые файлы'
}
for ($i = 1; $i -le 5; $i++) {
    Set-Content -LiteralPath (Join-Path $root ("hello{0}.exe" -f $i)) -Value ("MZ hello #$i") -Encoding ASCII
}
for ($i = 1; $i -le 6; $i++) {
    Set-Content -LiteralPath (Join-Path $root "locked\locked_$i.dat") -Value ("payload $i" * 40) -Encoding UTF8
}
Set-Content -LiteralPath (Join-Path $root 'README.txt') -Value 'DM-Boss test target' -Encoding UTF8

Write-Output 'меняю владельца и права...'
cmd /c "icacls $quote$root$quote /setowner ""NT AUTHORITY\SYSTEM"" /T /C /Q" | Out-Null
cmd /c "icacls $quote$root$quote /inheritance:r" | Out-Null
cmd /c "icacls $quote$root$quote /grant ""*S-1-5-18:(OI)(CI)F""" | Out-Null
cmd /c "icacls $quote$root$quote /grant ""*S-1-5-32-544:(OI)(CI)RX""" | Out-Null
cmd /c "icacls $quote$root$quote /grant ""${user}:(OI)(CI)RX""" | Out-Null
cmd /c "icacls $quote$root$quote /deny ""*S-1-5-32-544:(DE,DC)"" /T /C /Q" | Out-Null
cmd /c "icacls $quote$root$quote /deny ""${user}:(DE,DC)"" /T /C /Q" | Out-Null
cmd /c "attrib +R $quote$root\*.*$quote /S /D" | Out-Null

Write-Output 'запускаю процессы-держатели...'
$hdir = Join-Path $desktop 'engines'
$ready = $true
foreach ($n in @('dmguard.exe', 'dmlocker.exe')) {
    $src = Join-Path $hdir $n
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination $root -Force
    } else {
        $ready = $false
    }
}
if ($ready) {
    $argsGuard = @(($quote + $root + $quote), 'guard', '3')
    $argsWatch = @(($quote + $root + $quote), 'watch', '1')
    Start-Process -FilePath (Join-Path $root 'dmguard.exe') -ArgumentList $argsGuard -WindowStyle Hidden
    Start-Process -FilePath (Join-Path $root 'dmguard.exe') -ArgumentList $argsWatch -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $cnt = (Get-Process dmguard,dmlocker -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Output ('  процессов защиты: ' + $cnt)
} else {
    Write-Output '  dmguard/dmlocker не найдены в Desktop\engines, слой процессов пропущен'
}

Write-Output ''
Write-Output '=== ПРОВЕРКА ==='
try {
    $c = (Get-ChildItem -LiteralPath $root -Force -ErrorAction Stop | Measure-Object).Count
    Write-Output ("вход в папку: РАЗРЕШЁН, элементов в корне: " + $c)
} catch {
    Write-Output ('вход в папку: ЗАПРЕЩЁН - ' + $_.Exception.Message)
}
try {
    Remove-Item -LiteralPath (Join-Path $root 'hello1.exe') -Force -ErrorAction Stop
    Write-Output 'удаление файла: ПРОШЛО (плохо)'
} catch {
    Write-Output 'удаление файла: ЗАПРЕЩЕНО'
}
Write-Output ''
Write-Output 'Цель для DeleteMaster:'
Write-Output $root
Write-Output 'Снять: make-desktop-boss.ps1 -Remove'
