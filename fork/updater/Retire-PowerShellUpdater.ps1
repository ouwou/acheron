#Requires -Version 5.1
<#
.SYNOPSIS
    Switches from the PowerShell updater (Update-Acheron.ps1) to the one built into Acheron.

.DESCRIPTION
    Run this after the PowerShell updater has installed a build that includes acheron-updater.exe.
    It removes the "Acheron Updater" scheduled task and the "Acheron (auto-update)" Start Menu
    shortcut. Settings in %LOCALAPPDATA%\AcheronUpdater (where builds come from, the backup used
    by rollback) stay, since acheron-updater.exe uses them too. Afterwards, start Acheron from
    acheron.exe (pin it to the taskbar or Start); it checks for updates itself.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$dataDir = Join-Path $env:LOCALAPPDATA 'AcheronUpdater'
$state = Get-Content (Join-Path $dataDir 'state.json') -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json
$installDir = if ($state -and $state.InstallDir) { [string]$state.InstallDir } else { 'C:\bin\acheron' }

if (-not (Test-Path (Join-Path $installDir 'acheron-updater.exe'))) {
    throw "acheron-updater.exe isn't in $installDir yet. Update Acheron with Update-Acheron.ps1 first, then run this again."
}

Unregister-ScheduledTask -TaskName 'Acheron Updater' -Confirm:$false -ErrorAction SilentlyContinue
Write-Host "Removed the 'Acheron Updater' scheduled task (if it was there)."

$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Acheron (auto-update).lnk'
if (Test-Path $shortcut) { Remove-Item $shortcut -Force; Write-Host "Removed $shortcut" }

$copy = Join-Path $dataDir 'Update-Acheron.ps1'
if (Test-Path $copy) { Remove-Item $copy -Force; Write-Host "Removed the saved copy of Update-Acheron.ps1" }

Write-Host ""
Write-Host "Done. Start Acheron from $(Join-Path $installDir 'acheron.exe') and pin it; unpin the old shortcut if it's still pinned."
Write-Host "Acheron checks for updates on launch, every 6 hours, and from 'Check for Updates' in the tray menu or View menu."
