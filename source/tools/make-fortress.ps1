param(
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Запусти от имени администратора.'
}

$base = 'C:\ProgramData\DMShield'
$vault = Join-Path $base 'Vault'
$restore = Join-Path $base 'restore.ps1'
$taskName = 'DMShieldRestore'
$user = "$env:USERDOMAIN\$env:USERNAME"
$q = [char]34

if ($Remove) {
    Write-Output 'удаляю задачу восстановления...'
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Output 'убираю из Controlled Folder Access...'
    try { Remove-MpPreference -ControlledFolderAccessProtectedFolders $vault -ErrorAction Stop } catch { }
    try { Set-MpPreference -EnableControlledFolderAccess Disabled -ErrorAction Stop } catch { }
    Get-Process dmguard,dmlocker -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
    cmd /c "takeown /F $q$base$q /R /D Y" | Out-Null
    cmd /c "icacls $q$base$q /reset /T /C /Q" | Out-Null
    cmd /c "attrib -R -S -H $q$base$q /S /D" | Out-Null
    cmd /c "rd /S /Q $q$base$q" | Out-Null
    Write-Output ('крепость снесена: ' + (-not (Test-Path -LiteralPath $base)))
    return
}

Write-Output 'создаю структуру...'
New-Item -ItemType Directory -Force -Path $base, $vault, "$vault\bin", "$vault\core", "$vault\locked" | Out-Null

$sample = @(
    'C:\Users\Admin\OneDrive\Desktop\engines\DeleteEngine.Verify.dll'
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($sample) {
    $n = 0
    foreach ($dir in @("$vault\bin", "$vault\core")) {
        for ($i = 1; $i -le 100; $i++) {
            $n++
            Copy-Item -LiteralPath $sample -Destination (Join-Path $dir ("module_{0:d4}.dll" -f $n)) -Force
        }
    }
}
for ($i = 1; $i -le 5; $i++) {
    Set-Content -LiteralPath (Join-Path $vault ("hello{0}.exe" -f $i)) -Value ("MZ hello #$i") -Encoding ASCII
}
for ($i = 1; $i -le 6; $i++) {
    Set-Content -LiteralPath (Join-Path $vault "locked\locked_$i.dat") -Value ("payload $i" * 40) -Encoding UTF8
}
Set-Content -LiteralPath (Join-Path $vault 'README.txt') -Value 'DMShield fortress vault' -Encoding UTF8

Write-Output 'пишу сценарий восстановления...'
$restoreCode = @'
$vault = 'C:\ProgramData\DMShield\Vault'
if (-not (Test-Path -LiteralPath $vault)) {
    New-Item -ItemType Directory -Force -Path $vault | Out-Null
    Set-Content -LiteralPath (Join-Path $vault 'RESTORED.txt') -Value ("restored " + (Get-Date).ToString('s')) -Encoding UTF8
}
try { Add-MpPreference -ControlledFolderAccessProtectedFolders $vault -ErrorAction SilentlyContinue } catch { }
'@
[IO.File]::WriteAllText($restore, $restoreCode, (New-Object System.Text.UTF8Encoding($true)))

Write-Output 'ставлю ACL...'
cmd /c "icacls $q$vault$q /setowner ""NT AUTHORITY\SYSTEM"" /T /C /Q" | Out-Null
cmd /c "icacls $q$vault$q /inheritance:r" | Out-Null
cmd /c "icacls $q$vault$q /grant ""*S-1-5-18:(OI)(CI)F""" | Out-Null
cmd /c "icacls $q$vault$q /grant ""${user}:(OI)(CI)RX""" | Out-Null
cmd /c "icacls $q$vault$q /deny ""${user}:(DE,DC)"" /T /C /Q" | Out-Null
cmd /c "attrib +S +H $q$base$q" | Out-Null

Write-Output 'включаю Controlled Folder Access...'
Set-MpPreference -EnableControlledFolderAccess Enabled
Add-MpPreference -ControlledFolderAccessProtectedFolders $vault

Write-Output 'регистрирую задачу восстановления (SYSTEM, при входе)...'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File ' + $q + $restore + $q)
$trigger = New-ScheduledTaskTrigger -AtLogOn
$principalTask = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principalTask -Settings $settings | Out-Null

Write-Output 'запускаю процессы-держатели...'
$hdir = 'C:\Users\Admin\OneDrive\Desktop\engines'
$ok = $true
foreach ($n in @('dmguard.exe', 'dmlocker.exe')) {
    $src = Join-Path $hdir $n
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $base -Force } else { $ok = $false }
}
if ($ok) {
    Start-Process -FilePath (Join-Path $base 'dmguard.exe') -ArgumentList @(($q + $vault + $q), 'guard', '2') -WindowStyle Hidden
    Start-Process -FilePath (Join-Path $base 'dmguard.exe') -ArgumentList @(($q + $vault + $q), 'watch', '1') -WindowStyle Hidden
    Start-Sleep -Seconds 2
}

$pref = Get-MpPreference
Write-Output ''
Write-Output '=== КРЕПОСТЬ ГОТОВА ==='
Write-Output ('CFA: ' + $pref.EnableControlledFolderAccess)
Write-Output ('в списке защищённых: ' + ($pref.ControlledFolderAccessProtectedFolders -contains $vault))
Write-Output ('задача восстановления: ' + [bool](Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue))
Write-Output ('процессов защиты: ' + ((Get-Process dmguard,dmlocker -ErrorAction SilentlyContinue | Measure-Object).Count))
Write-Output ('путь: ' + $vault)
Write-Output ''
Write-Output 'Слои: драйвер Defender + ACL/владелец SYSTEM + 2 супервизора + watcher + задача SYSTEM при входе.'
Write-Output 'Если папку всё же снесут, она вернётся: сразу от watcher, после перезагрузки - от задачи.'
Write-Output 'Снять: make-fortress.ps1 -Remove'
