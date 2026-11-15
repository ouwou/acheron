#Requires -Version 5.1
<#
.SYNOPSIS
    Checks for new Acheron nightly builds and offers to install them.

.DESCRIPTION
    Acheron publishes a Windows build for every successful push to master
    (GitHub Actions workflow "build.yml", served through nightly.link).
    This script asks the GitHub API for the newest successful master run,
    compares it with the build it last installed, and if there is a newer
    one shows what changed and offers to update. Updating downloads that
    exact run's artifact, verifies its SHA-256 against GitHub, closes
    Acheron if it is running, swaps the files (keeping a backup of the
    previous build) and starts Acheron again.

    Your Acheron data (accounts, settings, cache) is not touched; only the
    program files the build ships are replaced.

.EXAMPLE
    .\Update-Acheron.ps1
    Check now and ask before updating.

.EXAMPLE
    .\Update-Acheron.ps1 -Register
    Check automatically at logon and once a day (only pops up when there is an update).

.EXAMPLE
    .\Update-Acheron.ps1 -CreateShortcut
    Add an "Acheron (auto-update)" Start Menu shortcut that checks, then launches Acheron.

.EXAMPLE
    .\Update-Acheron.ps1 -Rollback
    Put the previous build back.
#>
[CmdletBinding()]
param(
    # Folder containing acheron.exe. Remembered after the first run.
    [string]$InstallDir,

    # Which Windows artifact to install. Auto-detected from the current install if omitted.
    [ValidateSet('MinSizeRel', 'RelWithDebInfo', 'minimal', 'qt5-MinSizeRel')]
    [string]$Flavor,

    # Background mode: no "you're up to date" or error pop-ups, honour "Skip this build".
    [switch]$Quiet,

    # Install without asking.
    [switch]$AutoInstall,

    # Only report whether an update exists (exit code 10 = update available).
    [switch]$CheckOnly,

    # Start Acheron when done (used by the shortcut).
    [switch]$Launch,

    # Reinstall the latest build even if it is already installed.
    [switch]$Force,

    # Register a scheduled task that checks at logon and daily.
    [switch]$Register,
    [string]$DailyAt = '12:00',
    [switch]$Unregister,

    # Create a Start Menu shortcut that checks for updates, then launches Acheron.
    [switch]$CreateShortcut,

    # Restore the build that was installed before the last update.
    [switch]$Rollback,

    # Skip the SHA-256 check against GitHub's artifact digest.
    [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar on in 5.1
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Repo         = 'ouwou/acheron'
$WorkflowFile = 'build.yml'
$Branch       = 'master'
$TaskName     = 'Acheron Updater'
$UserAgent    = 'acheron-updater'
$DataDir      = Join-Path $env:LOCALAPPDATA 'AcheronUpdater'
$StatePath    = Join-Path $DataDir 'state.json'
$LogPath      = Join-Path $DataDir 'updater.log'
$BackupDir    = Join-Path $DataDir 'previous'
$StageDir     = Join-Path $DataDir 'staging'
$DownloadDir  = Join-Path $DataDir 'download'

New-Item -ItemType Directory -Force -Path $DataDir | Out-Null

# ---------------------------------------------------------------- helpers

function Write-Log([string]$Message) {
    if ((Test-Path $LogPath) -and (Get-Item $LogPath).Length -gt 512KB) {
        Move-Item $LogPath "$LogPath.old" -Force
    }
    $line = '{0:yyyy-MM-dd HH:mm:ss}  {1}' -f (Get-Date), $Message
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
    Write-Host $Message
}

function Read-State {
    $s = @{}
    if (Test-Path $StatePath) {
        try {
            $o = Get-Content $StatePath -Raw | ConvertFrom-Json
            foreach ($p in $o.PSObject.Properties) { $s[$p.Name] = $p.Value }
        } catch { Write-Log "State file unreadable, starting fresh: $_" }
    }
    return $s
}

function Save-State([hashtable]$State) {
    $State | ConvertTo-Json -Depth 5 | Set-Content -Path $StatePath -Encoding UTF8
}

function Invoke-GitHub([string]$Uri) {
    $headers = @{ 'User-Agent' = $UserAgent; 'Accept' = 'application/vnd.github+json' }
    if ($env:GITHUB_TOKEN) { $headers['Authorization'] = "Bearer $env:GITHUB_TOKEN" }
    Invoke-RestMethod -Uri $Uri -Headers $headers -TimeoutSec 30
}

function Invoke-Robocopy([string]$From, [string]$To, [string[]]$Mode) {
    $rcArgs = @($From.TrimEnd('\'), $To.TrimEnd('\')) + $Mode + @('/R:3', '/W:2', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
    & robocopy.exe @rcArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copy from '$From' to '$To' failed (robocopy exit code $LASTEXITCODE). A file may be in use." }
}

function Get-DefaultFlavor([string]$Dir) {
    if (Test-Path (Join-Path $Dir 'libssl-1_1-x64.dll')) { return 'qt5-MinSizeRel' }
    if ((Test-Path (Join-Path $Dir 'acheron.exe')) -and -not (Get-ChildItem $Dir -Filter 'avcodec-*.dll' -ErrorAction SilentlyContinue)) {
        return 'minimal'
    }
    return 'MinSizeRel'
}

# ---------------------------------------------------------------- UI

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

function Show-Message([string]$Text, [string]$Icon = 'Information') {
    # Invisible topmost owner so the box comes to the front even when started by Task Scheduler
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true; $owner.ShowInTaskbar = $false; $owner.Opacity = 0
    $owner.StartPosition = 'CenterScreen'; $owner.Size = New-Object System.Drawing.Size(1, 1)
    $owner.Show(); $owner.Activate()
    [void][System.Windows.Forms.MessageBox]::Show($owner, $Text, 'Acheron Updater',
        [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]$Icon)
    $owner.Dispose()
}

function Show-UpdateDialog($Latest, [string]$Installed, [string]$Changes, [bool]$Running, [string]$ExePath) {
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Acheron update available'
    $form.StartPosition = 'CenterScreen'
    $form.TopMost = $true
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false; $form.MinimizeBox = $false
    $form.ClientSize = New-Object System.Drawing.Size(540, 380)
    $form.Font = New-Object System.Drawing.Font('Segoe UI', 9)
    if (Test-Path $ExePath) { try { $form.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon($ExePath) } catch {} }

    $label = New-Object System.Windows.Forms.Label
    $label.Location = New-Object System.Drawing.Point(12, 12)
    $label.Size = New-Object System.Drawing.Size(516, 54)
    $text = "A new Acheron nightly build is available: $($Latest.Label)`r`nInstalled: $Installed"
    if ($Running) { $text += "`r`nAcheron is running; it will be closed and reopened." }
    $label.Text = $text

    $box = New-Object System.Windows.Forms.TextBox
    $box.Multiline = $true; $box.ReadOnly = $true; $box.ScrollBars = 'Vertical'; $box.WordWrap = $true
    $box.BackColor = [System.Drawing.SystemColors]::Window
    $box.Location = New-Object System.Drawing.Point(12, 70)
    $box.Size = New-Object System.Drawing.Size(516, 258)
    $box.Text = $Changes

    $skip = New-Object System.Windows.Forms.Button
    $skip.Text = 'Skip this build'; $skip.DialogResult = 'Ignore'
    $skip.Location = New-Object System.Drawing.Point(12, 340); $skip.Size = New-Object System.Drawing.Size(120, 28)

    $later = New-Object System.Windows.Forms.Button
    $later.Text = 'Later'; $later.DialogResult = 'Cancel'
    $later.Location = New-Object System.Drawing.Point(312, 340); $later.Size = New-Object System.Drawing.Size(96, 28)

    $update = New-Object System.Windows.Forms.Button
    $update.Text = 'Update now'; $update.DialogResult = 'OK'
    $update.Location = New-Object System.Drawing.Point(416, 340); $update.Size = New-Object System.Drawing.Size(112, 28)

    $form.Controls.AddRange(@($label, $box, $skip, $later, $update))
    $form.AcceptButton = $update
    $form.CancelButton = $later
    $form.Add_Shown({ $form.Activate(); $update.Focus() })
    $result = $form.ShowDialog()
    $form.Dispose()
    return $result
}

# ---------------------------------------------------------------- GitHub / nightly.link

function Get-LatestBuild([string]$Artifact) {
    # Same selection nightly.link uses: newest successful push run of build.yml on master.
    try {
        $r = Invoke-GitHub "https://api.github.com/repos/$Repo/actions/workflows/$WorkflowFile/runs?branch=$Branch&event=push&status=success&per_page=1"
        $run = @($r.workflow_runs) | Select-Object -First 1
        if ($run) {
            $date = [datetime]$run.updated_at
            return [pscustomobject]@{
                RunId   = [string]$run.id
                Sha     = [string]$run.head_sha
                Message = [string]$run.head_commit.message
                Url     = [string]$run.html_url
                Label   = "build #$($run.run_number), $($date.ToString('g')) (commit $($run.head_sha.Substring(0, 7)))"
            }
        }
    } catch {
        Write-Log "GitHub API unavailable ($($_.Exception.Message)); falling back to nightly.link."
    }

    $page = Invoke-WebRequest -UseBasicParsing -UserAgent $UserAgent -TimeoutSec 30 `
        -Uri "https://nightly.link/$Repo/workflows/$($WorkflowFile -replace '\.ya?ml$', '')/$Branch/$Artifact"
    if ($page.Content -notmatch '/actions/runs/(\d+)/') { throw 'Could not find the latest build on nightly.link.' }
    $id = $Matches[1]
    return [pscustomobject]@{
        RunId = $id; Sha = $null; Message = $null
        Url   = "https://github.com/$Repo/actions/runs/$id"
        Label = "run $id"
    }
}

function Get-ChangeSummary($Latest, [string]$InstalledSha) {
    $lines = New-Object System.Collections.Generic.List[string]
    if ($InstalledSha -and $Latest.Sha -and $InstalledSha -ne $Latest.Sha) {
        try {
            $cmp = Invoke-GitHub "https://api.github.com/repos/$Repo/compare/$InstalledSha...$($Latest.Sha)"
            $commits = @($cmp.commits)
            [array]::Reverse($commits)   # newest first
            $lines.Add("$($cmp.total_commits) new commit(s) since your build:")
            $lines.Add('')
            foreach ($c in ($commits | Select-Object -First 30)) {
                $subject = ($c.commit.message -split "`n")[0].Trim()
                $lines.Add("- $subject  [$($c.sha.Substring(0, 7))]")
            }
            if ($cmp.total_commits -gt 30) { $lines.Add("- ...and $($cmp.total_commits - 30) more") }
        } catch { Write-Log "Could not fetch commit list: $($_.Exception.Message)" }
    }
    if ($lines.Count -eq 0 -and $Latest.Message) {
        $lines.Add('Latest commit:')
        $lines.Add('')
        foreach ($l in ($Latest.Message -split "`n")) { $lines.Add($l.TrimEnd()) }
    }
    $lines.Add('')
    $lines.Add("Build details: $($Latest.Url)")
    return ($lines -join "`r`n")
}

function Get-ArtifactDigest([string]$RunId, [string]$Artifact) {
    try {
        $r = Invoke-GitHub "https://api.github.com/repos/$Repo/actions/runs/$RunId/artifacts?name=$Artifact"
        $a = @($r.artifacts) | Select-Object -First 1
        if ($a -and $a.PSObject.Properties['digest'] -and $a.digest) { return ([string]$a.digest -replace '^sha256:', '').ToLower() }
    } catch { Write-Log "Could not fetch artifact digest: $($_.Exception.Message)" }
    return $null
}

function Get-Build($Latest, [string]$Artifact) {
    New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
    Get-ChildItem $DownloadDir -Filter '*.zip' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    $zip = Join-Path $DownloadDir "$Artifact-$($Latest.RunId).zip"
    $url = "https://nightly.link/$Repo/actions/runs/$($Latest.RunId)/$Artifact.zip"

    Write-Log "Downloading $url"
    for ($try = 1; ; $try++) {
        try {
            Invoke-WebRequest -UseBasicParsing -UserAgent $UserAgent -Uri $url -OutFile $zip -TimeoutSec 900
            break
        } catch {
            if ($try -ge 3) { throw "Download failed: $($_.Exception.Message)" }
            Start-Sleep -Seconds (5 * $try)
        }
    }

    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    $expected = if ($NoVerify) { $null } else { Get-ArtifactDigest $Latest.RunId $Artifact }
    if ($expected) {
        if ($hash -ne $expected) { throw "Downloaded file failed verification (SHA-256 $hash, GitHub says $expected). Run with -NoVerify to bypass." }
        Write-Log "SHA-256 verified against GitHub: $hash"
    } else {
        Write-Log "SHA-256 $hash (GitHub digest not available to verify against)"
    }

    if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $StageDir -Force
    Remove-Item $zip -Force

    $root = $StageDir
    if (-not (Test-Path (Join-Path $root 'acheron.exe'))) {
        $sub = Get-ChildItem $StageDir -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'acheron.exe') } | Select-Object -First 1
        if (-not $sub) { throw 'The downloaded archive does not contain acheron.exe.' }
        $root = $sub.FullName
    }
    return $root
}

# ---------------------------------------------------------------- install

function Get-AcheronProcesses([string]$ExePath) {
    $full = [IO.Path]::GetFullPath($ExePath)
    @(Get-Process -Name 'acheron' -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -ieq $full) } catch { $false }
    })
}

function Stop-Acheron($Processes) {
    foreach ($p in $Processes) {
        $closed = $false
        try { $closed = $p.CloseMainWindow() } catch {}
        # Minimised to tray means no main window to close; give it a moment, then end it.
        $wait = if ($closed) { 15000 } else { 3000 }
        if (-not $p.WaitForExit($wait)) {
            Write-Log "Acheron (PID $($p.Id)) did not exit; ending it."
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            [void]$p.WaitForExit(10000)
        }
    }
    Start-Sleep -Milliseconds 750   # let Windows release the file handles
}

function Install-Build([string]$Source, [string]$Target, $OldManifest) {
    $srcFull = (Resolve-Path $Source).Path.TrimEnd('\')
    $manifest = @(Get-ChildItem $srcFull -Recurse -File | ForEach-Object { $_.FullName.Substring($srcFull.Length + 1) })

    if (Test-Path $BackupDir) { Remove-Item $BackupDir -Recurse -Force }
    $hadInstall = Test-Path (Join-Path $Target 'acheron.exe')
    if ($hadInstall) { Invoke-Robocopy $Target $BackupDir @('/MIR') }

    try {
        New-Item -ItemType Directory -Force -Path $Target | Out-Null
        Invoke-Robocopy $srcFull $Target @('/E')

        # Remove files an earlier build shipped that this one no longer does.
        # Only files the updater itself installed are candidates, so anything else in the folder is left alone.
        if ($OldManifest) {
            $keep = @{}
            foreach ($f in $manifest) { $keep[$f.ToLowerInvariant()] = $true }
            foreach ($old in @($OldManifest)) {
                if (-not $keep.ContainsKey(([string]$old).ToLowerInvariant())) {
                    $p = Join-Path $Target $old
                    if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
                }
            }
        }
        if (-not (Test-Path (Join-Path $Target 'acheron.exe'))) { throw 'acheron.exe missing after copy.' }
    } catch {
        Write-Log "Install failed: $($_.Exception.Message)"
        if ($hadInstall) {
            Write-Log 'Restoring the previous build.'
            Invoke-Robocopy $BackupDir $Target @('/E')
        }
        throw
    }
    Remove-Item $StageDir -Recurse -Force -ErrorAction SilentlyContinue
    return , $manifest
}

function Start-Acheron([string]$ExePath) {
    if (Test-Path $ExePath) { Start-Process -FilePath $ExePath -WorkingDirectory (Split-Path $ExePath) }
}

# ---------------------------------------------------------------- setup actions

function Get-InstalledScriptPath {
    # Keep a copy in %LOCALAPPDATA% so the task/shortcut don't depend on where this file was downloaded to.
    $self = Join-Path $DataDir 'Update-Acheron.ps1'
    if ($PSCommandPath -and ([IO.Path]::GetFullPath($PSCommandPath) -ine [IO.Path]::GetFullPath($self))) {
        Copy-Item $PSCommandPath $self -Force
    }
    return $self
}

function Register-UpdaterTask([string]$Dir, [string]$Flv) {
    $self = Get-InstalledScriptPath
    $arg = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$self`" -Quiet -InstallDir `"$Dir`" -Flavor $Flv"
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
    $user = "$env:USERDOMAIN\$env:USERNAME"
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 30) -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $daily = New-ScheduledTaskTrigger -Daily -At $DailyAt
    $logon = New-ScheduledTaskTrigger -AtLogOn -User $user
    $logon.Delay = 'PT2M'
    $desc = 'Checks for new Acheron nightly builds and offers to install them.'
    try {
        Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger @($logon, $daily) -Settings $settings `
            -Principal $principal -Description $desc -Force | Out-Null
        Write-Log "Scheduled task '$TaskName' registered: at logon and daily at $DailyAt."
    } catch {
        # Some systems only allow logon triggers from an elevated prompt.
        Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $daily -Settings $settings `
            -Principal $principal -Description $desc -Force | Out-Null
        Write-Log "Scheduled task '$TaskName' registered: daily at $DailyAt (logon trigger needs an elevated prompt)."
    }
}

function New-LaunchShortcut([string]$Dir, [string]$Flv) {
    $self = Get-InstalledScriptPath
    $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Acheron (auto-update).lnk'
    $shell = New-Object -ComObject WScript.Shell
    $sc = $shell.CreateShortcut($lnk)
    $sc.TargetPath = (Get-Command powershell.exe).Source
    $sc.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$self`" -Launch -Quiet -InstallDir `"$Dir`" -Flavor $Flv"
    $sc.WorkingDirectory = $Dir
    $sc.IconLocation = "$(Join-Path $Dir 'acheron.exe'),0"
    $sc.WindowStyle = 7
    $sc.Description = 'Check for Acheron updates, then start Acheron'
    $sc.Save()
    Write-Log "Shortcut created: $lnk"
}

# ---------------------------------------------------------------- main

function Invoke-Main {
    $state = Read-State

    if (-not $InstallDir) {
        if ($state['InstallDir']) { $script:InstallDir = [string]$state['InstallDir'] }
        elseif ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot 'acheron.exe'))) { $script:InstallDir = $PSScriptRoot }
        else { $script:InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\Acheron' }
    }
    $script:InstallDir = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    if (-not $Flavor) {
        if ($state['Flavor']) { $script:Flavor = [string]$state['Flavor'] } else { $script:Flavor = Get-DefaultFlavor $InstallDir }
    }
    $artifact = "acheron-windows-$Flavor"
    $exe = Join-Path $InstallDir 'acheron.exe'
    $state['InstallDir'] = $InstallDir
    $state['Flavor'] = $Flavor

    if ($Unregister) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
        Write-Log "Scheduled task '$TaskName' removed."
        return 0
    }
    if ($Register -or $CreateShortcut) {
        Save-State $state
        if ($Register) { Register-UpdaterTask $InstallDir $Flavor }
        if ($CreateShortcut) { New-LaunchShortcut $InstallDir $Flavor }
        return 0
    }

    if ($Rollback) {
        if (-not (Test-Path (Join-Path $BackupDir 'acheron.exe'))) { throw 'There is no previous build to roll back to.' }
        $running = Get-AcheronProcesses $exe
        if ($running) { Stop-Acheron $running }
        Invoke-Robocopy $BackupDir $InstallDir @('/E')
        $bad = $state['RunId']
        $state['RunId'] = $state['PreviousRunId']; $state['Sha'] = $state['PreviousSha']
        $state['PreviousRunId'] = $null; $state['PreviousSha'] = $null
        $state['SkippedRunId'] = $bad   # don't offer the build you just rolled back from again
        Save-State $state
        Write-Log "Rolled back to the previous build in $InstallDir."
        if ($running -or $Launch) { Start-Acheron $exe }
        if (-not $Quiet) { Show-Message 'Acheron has been rolled back to the previous build.' }
        return 0
    }

    Write-Log "Checking for updates ($artifact, $InstallDir)"
    $latest = Get-LatestBuild $artifact
    $installedRun = [string]$state['RunId']
    $haveExe = Test-Path $exe

    if (-not $Force -and $haveExe -and $installedRun -eq $latest.RunId) {
        Write-Log "Up to date ($($latest.Label))."
        if ($Launch) { Start-Acheron $exe }
        elseif (-not $Quiet -and -not $CheckOnly) { Show-Message "Acheron is up to date.`n`n$($latest.Label)" }
        return 0
    }

    Write-Log "Update available: $($latest.Label)"
    if ($CheckOnly) { return 10 }
    if ($Quiet -and -not $Force -and [string]$state['SkippedRunId'] -eq $latest.RunId) {
        Write-Log 'This build was skipped earlier; not asking again.'
        if ($Launch) { Start-Acheron $exe }
        return 0
    }

    $running = Get-AcheronProcesses $exe
    if (-not $AutoInstall) {
        $installedLabel = if (-not $haveExe) { "not installed in $InstallDir" }
                          elseif ($installedRun) { "run $installedRun" + $(if ($state['Sha']) { " (commit $(([string]$state['Sha']).Substring(0, 7)))" } else { '' }) }
                          else { 'unknown (first time using the updater)' }
        $changes = Get-ChangeSummary $latest ([string]$state['Sha'])
        $choice = Show-UpdateDialog $latest $installedLabel $changes ([bool]$running) $exe
        if ($choice -eq 'Ignore') {
            $state['SkippedRunId'] = $latest.RunId; Save-State $state
            Write-Log 'Skipped this build.'
            if ($Launch) { Start-Acheron $exe }
            return 0
        }
        if ($choice -ne 'OK') {
            Write-Log 'Update postponed.'
            if ($Launch) { Start-Acheron $exe }
            return 0
        }
        $running = Get-AcheronProcesses $exe   # may have changed while the dialog was open
    }

    $staged = Get-Build $latest $artifact
    if ($running) { Write-Log 'Closing Acheron.'; Stop-Acheron $running }
    $manifest = Install-Build $staged $InstallDir $state['Manifest']

    $state['PreviousRunId'] = $state['RunId']; $state['PreviousSha'] = $state['Sha']
    $state['RunId'] = $latest.RunId
    $state['Sha'] = $latest.Sha
    $state['InstalledAt'] = (Get-Date).ToString('o')
    $state['Manifest'] = $manifest
    $state['SkippedRunId'] = $null
    Save-State $state
    Write-Log "Installed $($latest.Label) to $InstallDir."

    if ($running -or $Launch) { Start-Acheron $exe }
    elseif (-not $Quiet) { Show-Message "Acheron has been updated.`n`n$($latest.Label)" }
    return 0
}

$mutex = New-Object System.Threading.Mutex($false, 'Local\AcheronUpdater')
if (-not $mutex.WaitOne(0)) { Write-Host 'Another Acheron Updater is already running.'; exit 0 }
$code = 0
try {
    $code = Invoke-Main | Select-Object -Last 1
} catch {
    $code = 1
    Write-Log "ERROR: $($_.Exception.Message)"
    if (-not $Quiet) { Show-Message "Acheron update failed:`n`n$($_.Exception.Message)`n`nLog: $LogPath" 'Error' }
    if ($Launch -and $InstallDir) { Start-Acheron (Join-Path $InstallDir 'acheron.exe') }
} finally {
    $mutex.ReleaseMutex(); $mutex.Dispose()
}
exit $code
