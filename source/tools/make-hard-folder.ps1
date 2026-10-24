param(
    [string]$Root = "C:\ProgramData\DMVault",
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Запусти от имени администратора.'
}

$target = Join-Path $Root 'Protected'

if ($Remove) {
    Write-Output 'снимаю защиту...'
    Get-Process dmguard,dmlocker -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
    cmd /c "takeown /F `"$Root`" /R /D Y" | Out-Null
    cmd /c "icacls `"$Root`" /reset /T /C /Q" | Out-Null
    cmd /c "attrib -R -S -H `"$Root`" /S /D" | Out-Null
    cmd /c "rd /S /Q `"$Root`"" | Out-Null
    Write-Output ('удалено: ' + (-not (Test-Path -LiteralPath $Root)))
    return
}

New-Item -ItemType Directory -Force -Path $target, "$target\bin", "$target\core", "$target\locked" | Out-Null

$engineDll = @(
    "$env:USERPROFILE\OneDrive\Desktop\engines\DeleteEngine.Verify.dll",
    "$env:USERPROFILE\Desktop\engines\DeleteEngine.Verify.dll"
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

Write-Output 'наполняю...'
if ($engineDll) {
    $n = 0
    foreach ($dir in @("$target\bin", "$target\core")) {
        for ($i = 1; $i -le 100; $i++) {
            $n++
            Copy-Item -LiteralPath $engineDll -Destination (Join-Path $dir ("module_{0:d4}.dll" -f $n)) -Force
        }
    }
} else {
    Write-Output '  образец DLL не найден, кладу только текстовые файлы'
}
for ($i = 1; $i -le 5; $i++) {
    Set-Content -LiteralPath (Join-Path $target ("hello{0}.exe" -f $i)) -Value ("MZ hello #$i") -Encoding ASCII
}
for ($i = 1; $i -le 6; $i++) {
    Set-Content -LiteralPath (Join-Path $target "locked\locked_$i.dat") -Value ("payload $i" * 40) -Encoding UTF8
}
Set-Content -LiteralPath (Join-Path $target 'README.txt') -Value 'protected folder test target' -Encoding UTF8

Write-Output 'включаю атрибуты...'
Get-ChildItem -LiteralPath $target -Recurse -File -Force | ForEach-Object {
    try { $_.Attributes = $_.Attributes -bor [IO.FileAttributes]::ReadOnly } catch { }
}

Write-Output 'включаю ACL и меняю владельца на SYSTEM...'
cmd /c "icacls `"$Root`" /setowner `"NT AUTHORITY\SYSTEM`" /T /C /Q" | Out-Null
cmd /c "icacls `"$target`" /inheritance:r" | Out-Null
cmd /c "icacls `"$target`" /grant `"*S-1-5-18:(OI)(CI)F`"" | Out-Null
cmd /c "icacls `"$target`" /deny `"*S-1-1-0:(OI)(CI)(DE,DC)`"" | Out-Null
# deny WRITE_DAC не ставлю: иначе папку нельзя будет снять даже админу
cmd /c "attrib +S +H `"$Root`"" | Out-Null

Write-Output 'запускаю процессы-держатели...'
$guard = Join-Path $Root 'dmguard.exe'
$locker = Join-Path $Root 'dmlocker.exe'
$helpers = @(
    "$env:USERPROFILE\OneDrive\Desktop\engines\dmguard.exe",
    "$env:USERPROFILE\OneDrive\Desktop\dmguard.exe"
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($helpers) {
    $hdir = Split-Path $helpers
    foreach ($n in @("dmguard.exe", "dmlocker.exe")) {
        $src = Join-Path $hdir $n
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $Root -Force }
    }
}
if ((Test-Path -LiteralPath $guard) -and (Test-Path -LiteralPath $locker)) {
    Start-Process -FilePath $guard -ArgumentList @("`"$target`"", 'guard', '3') -WindowStyle Hidden
    Start-Process -FilePath $guard -ArgumentList @("`"$target`"", 'watch', '1') -WindowStyle Hidden
    Start-Sleep -Seconds 2
} else {
    Write-Output '  хелперы dmguard/dmlocker не найдены, слой процессов пропущен'
}

Write-Output ''
Write-Output '=== ПРОВЕРКА ==='
$vol = Get-Item -LiteralPath $target -Force
Write-Output ('владелец: ' + (cmd /c "dir /q `"$target`"" 2>&1 | Select-String 'SYSTEM' | Select-Object -First 1))
try {
    $null = Get-ChildItem -LiteralPath $target -Force -ErrorAction Stop
    Write-Output 'листинг: РАЗРЕШЁН (значит, ACL не закрыл чтение)'
} catch {
    Write-Output 'листинг: ЗАПРЕЩЁН (' + $_.Exception.Message + ')'
}
try {
    Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
    Write-Output 'УДАЛЕНИЕ: ПРОШЛО — защиты нет'
} catch {
    Write-Output 'удаление: ЗАПРЕЩЕНО (' + $_.Exception.Message + ')'
}
Write-Output ''
Write-Output ('Цель для DeleteMaster: ' + $target)
Write-Output 'Снять: make-hard-folder.ps1 -Remove'
