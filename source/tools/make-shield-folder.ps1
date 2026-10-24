param(
    [string]$Path = "C:\DM-Shield\Vault",
    [switch]$Remove,
    [switch]$KeepCfaOn
)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Запусти от имени администратора.'
}

$q = [char]34
$user = "$env:USERDOMAIN\$env:USERNAME"

if ($Remove) {
    Write-Output 'убираю папку из защищённых...'
    try { Remove-MpPreference -ControlledFolderAccessProtectedFolders $Path -ErrorAction Stop } catch { Write-Output ('  ' + $_.Exception.Message) }
    if (-not $KeepCfaOn) {
        Write-Output 'выключаю Controlled Folder Access...'
        try { Set-MpPreference -EnableControlledFolderAccess Disabled -ErrorAction Stop } catch { Write-Output ('  ' + $_.Exception.Message) }
    }
    Get-Process dmguard,dmlocker -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
    cmd /c "takeown /F $q$Path$q /R /D Y" | Out-Null
    cmd /c "icacls $q$Path$q /reset /T /C /Q" | Out-Null
    cmd /c "rd /S /Q $q$Path$q" | Out-Null
    Write-Output ('папка удалена: ' + (-not (Test-Path -LiteralPath $Path)))
    return
}

Write-Output 'проверяю Defender...'
$status = Get-MpComputerStatus -ErrorAction SilentlyContinue
if (-not $status) { throw 'Defender недоступен: без него Controlled Folder Access не работает.' }
Write-Output ('  антивирус активен: ' + $status.AntivirusEnabled + ', realtime: ' + $status.RealTimeProtectionEnabled)

Write-Output 'создаю папку...'
New-Item -ItemType Directory -Force -Path $Path, "$Path\bin", "$Path\core", "$Path\locked" | Out-Null

$sample = @(
    'C:\Users\Admin\OneDrive\Desktop\engines\DeleteEngine.Verify.dll',
    'C:\Users\Admin\Desktop\engines\DeleteEngine.Verify.dll'
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if ($sample) {
    $n = 0
    foreach ($dir in @("$Path\bin", "$Path\core")) {
        for ($i = 1; $i -le 100; $i++) {
            $n++
            Copy-Item -LiteralPath $sample -Destination (Join-Path $dir ("module_{0:d4}.dll" -f $n)) -Force
        }
    }
} else {
    Write-Output '  образец DLL не найден, кладу текстовые файлы'
}
for ($i = 1; $i -le 5; $i++) {
    Set-Content -LiteralPath (Join-Path $Path ("hello{0}.exe" -f $i)) -Value ("MZ hello #$i") -Encoding ASCII
}
for ($i = 1; $i -le 6; $i++) {
    Set-Content -LiteralPath (Join-Path $Path "locked\locked_$i.dat") -Value ("payload $i" * 40) -Encoding UTF8
}
Set-Content -LiteralPath (Join-Path $Path 'README.txt') -Value 'DM-Shield vault: protected by Controlled Folder Access' -Encoding UTF8

Write-Output 'включаю Controlled Folder Access...'
Set-MpPreference -EnableControlledFolderAccess Enabled
Write-Output 'добавляю папку в защищённые...'
Add-MpPreference -ControlledFolderAccessProtectedFolders $Path

Write-Output 'проверяю настройки...'
$pref = Get-MpPreference
$cfa = $pref.EnableControlledFolderAccess
$listed = $pref.ControlledFolderAccessProtectedFolders -contains $Path

Write-Output ''
Write-Output '=== СТЕНД ГОТОВ ==='
Write-Output ('CFA: ' + $cfa)
Write-Output ('папка в списке защищённых: ' + $listed)
Write-Output ('путь: ' + $Path)
Write-Output ''
Write-Output 'Важно: защита работает на уровне драйвера Defender.'
Write-Output 'DeleteMaster не подписан и не в белом списке, поэтому запись и удаление внутри будут блокироваться.'
Write-Output 'Если добавить его в ControlledFolderAccessAllowedApplications - защита снимется.'
Write-Output 'CFA также защищает Рабочий стол, Документы, Изображения: если начнут падать другие программы,'
Write-Output 'это она. Снять: make-shield-folder.ps1 -Remove'
