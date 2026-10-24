param(
    [string]$Path = "$env:USERPROFILE\OneDrive\Desktop\DM-Boss",
    [switch]$Unlock
)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Запусти от имени администратора.'
}

if (-not (Test-Path -LiteralPath $Path)) {
    throw "Нет такой папки: $Path"
}

$q = [char]34
$user = "$env:USERDOMAIN\$env:USERNAME"

if ($Unlock) {
    Write-Output 'снимаю лок...'
    cmd /c "takeown /F $q$Path$q /R /D Y" | Out-Null
    cmd /c "icacls $q$Path$q /reset /T /C /Q" | Out-Null
    cmd /c "icacls $q$Path$q /grant ""${user}:(OI)(CI)F""" | Out-Null
    cmd /c "attrib -R -S -H $q$Path$q /S /D" | Out-Null
    Write-Output 'лок снят, доступ вернул'
    return
}

Write-Output 'ставлю владельца SYSTEM...'
cmd /c "icacls $q$Path$q /setowner ""NT AUTHORITY\SYSTEM"" /T /C /Q" | Out-Null

Write-Output 'снимаю наследование, оставляю только SYSTEM...'
cmd /c "icacls $q$Path$q /inheritance:r" | Out-Null
cmd /c "icacls $q$Path$q /grant ""*S-1-5-18:(OI)(CI)F""" | Out-Null

Write-Output 'закрываю вход и удаление всем остальным...'
cmd /c "icacls $q$Path$q /deny ""*S-1-5-11:(RD,DE,DC)"" /T /C /Q" | Out-Null
cmd /c "icacls $q$Path$q /deny ""*S-1-5-32-544:(RD,DE,DC)"" /T /C /Q" | Out-Null
cmd /c "attrib +S +H $q$Path$q" | Out-Null

Write-Output ''
Write-Output '=== ПРОВЕРКА ==='
try {
    $null = Get-ChildItem -LiteralPath $Path -Force -ErrorAction Stop
    Write-Output 'вход: РАЗРЕШЁН (лок не встал)'
} catch {
    Write-Output 'вход: ЗАПРЕЩЁН'
}
try {
    $null = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    Write-Output 'объект виден системе координат: да'
} catch {
    Write-Output 'объект даже не читается атрибутами'
}
Write-Output ''
Write-Output 'Цель для DeleteMaster:'
Write-Output $Path
Write-Output 'Снять лок: lock-system-only.ps1 -Unlock'
