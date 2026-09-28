<#
.SYNOPSIS
    Copy every irreplaceable S2MP-Mod work product to a destination on your local PC.

.DESCRIPTION
    Run this from wherever the work happens -- a Hyper-V guest, a second Windows
    install, or the machine itself -- pointing -Destination at a path that lives on
    the PC you want the files to end up on (a mapped share, a second drive, a NAS).

    WHAT IT COPIES, and why each one is irreplaceable:

      repo    the source tree. Git protects the history, but the working branch is
              deliberately not pushed anywhere, so the only copy is on disk.
      IDB     s2x_dump.exe.i64 -- months of function naming. If this is lost, every
              previously mapped address has to be re-derived. It is the single most
              valuable file in the project.
      demos   main/demo/*.demo and demos/*.dm_s2. A recording cannot be regenerated
              without replaying the same match, and public-match recordings in
              particular took real effort to obtain.
      logs    main/s2mp_console.log -- the measurements the findings rest on.
      config  s2mp_bans.txt and s2mp-mod.cfg.

    ⚠ IDB SAFETY: IDA keeps the OPEN database unpacked in loose
    .id0/.id1/.id2/.nam/.til files and only repacks them into the .i64 on a CLEAN
    CLOSE. A hard kill, a crash or a reboot leaves the .i64 STALE. This script checks
    the loose files' timestamps against the .i64 and refuses to present a stale .i64
    as a good backup -- it copies the loose files too, and says so.

.PARAMETER Destination
    Root folder on the local PC to copy into. Created if missing.
    e.g. \\HOSTPC\S2Backup   or   E:\S2-Workspace-Backup

.PARAMETER RepoPath
    The S2MP-Mod checkout. Defaults to two levels above this script.

.PARAMETER GamePath
    The WWII install (for demos and logs).

.PARAMETER IdbPath
    The .i64 database. Defaults to the one inside GamePath.

.PARAMETER WhatIf
    Show what would be copied without copying.

.EXAMPLE
    .\Sync-Workspace.ps1 -Destination E:\S2-Workspace-Backup

.EXAMPLE
    .\Sync-Workspace.ps1 -Destination \\HOSTPC\S2Backup -WhatIf
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string] $Destination,

    [string] $RepoPath = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),

    [string] $GamePath = 'E:\SteamLibrary\steamapps\common\Call of Duty WWII',

    [string] $IdbPath
)

$ErrorActionPreference = 'Stop'
$script:Problems = @()

function Write-Section { param([string]$Text) Write-Host "`n=== $Text ===" -ForegroundColor Cyan }
function Write-Ok      { param([string]$Text) Write-Host "  [ok]   $Text" -ForegroundColor Green }
function Write-Warn    { param([string]$Text) Write-Host "  [warn] $Text" -ForegroundColor Yellow; $script:Problems += $Text }
function Write-Skip    { param([string]$Text) Write-Host "  [skip] $Text" -ForegroundColor DarkGray }

# robocopy returns 0-7 for success (bits mean copied/extra/mismatch); >=8 is a real
# failure. Treating any non-zero as failure is the classic mistake here.
function Invoke-Robocopy {
    param([string]$Source, [string]$Dest, [string[]]$Files = @('*.*'), [string[]]$Extra = @())

    if (-not (Test-Path -LiteralPath $Source)) { Write-Skip "$Source (not present)"; return }
    if ($PSCmdlet.ShouldProcess($Source, "copy to $Dest")) {
        $args = @($Source, $Dest) + $Files + @('/E', '/R:1', '/W:1', '/NFL', '/NDL', '/NJH', '/NJS', '/NP') + $Extra
        & robocopy.exe @args | Out-Null
        if ($LASTEXITCODE -ge 8) { Write-Warn "robocopy failed ($LASTEXITCODE): $Source" }
        else {
            $n = (Get-ChildItem -LiteralPath $Dest -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count
            Write-Ok "$Source  ->  $Dest  ($n file(s) present)"
        }
    } else {
        Write-Host "  [whatif] $Source -> $Dest" -ForegroundColor DarkCyan
    }
}

Write-Host "S2MP-Mod workspace sync" -ForegroundColor White
Write-Host "destination : $Destination"

if (-not (Test-Path -LiteralPath $Destination)) {
    if ($PSCmdlet.ShouldProcess($Destination, 'create destination')) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    }
}

$stamp = Get-Date -Format 'yyyy-MM-dd_HHmm'

# ---------------------------------------------------------------- repo ----
Write-Section 'Source tree'
if (Test-Path -LiteralPath $RepoPath) {
    # /XD excludes: build output and dependency checkouts are regenerable and huge.
    # NOTE submodule .git entries are FILES, not directories, so /XD cannot exclude
    # them -- that is deliberate, they are small and worth keeping.
    Invoke-Robocopy -Source $RepoPath -Dest (Join-Path $Destination 'repo') `
        -Extra @('/XD', 'bin', 'build', '.vs', 'obj', '/XF', '*.pdb', '*.obj', '*.ilk')

    Push-Location $RepoPath
    try {
        $branch = (& git rev-parse --abbrev-ref HEAD 2>$null)
        $dirty  = (& git status --porcelain 2>$null)
        if ($branch) {
            Write-Ok "branch '$branch', $(( $dirty | Measure-Object ).Count) uncommitted change(s)"
            if ($dirty) { Write-Warn "the repo has uncommitted changes -- the copy is the only record of them" }
        }
    } catch { } finally { Pop-Location }
} else {
    Write-Warn "repo not found at $RepoPath"
}

# ----------------------------------------------------------------- IDB ----
Write-Section 'IDA database (the most valuable artefact)'
if (-not $IdbPath) { $IdbPath = Join-Path $GamePath 's2x_dump.exe.i64' }
$idbDir  = Split-Path -Parent $IdbPath
$idbBase = [IO.Path]::GetFileNameWithoutExtension($IdbPath)

if (Test-Path -LiteralPath $IdbPath) {
    $i64 = Get-Item -LiteralPath $IdbPath
    Write-Host ("  .i64 last written : {0}  ({1:N0} MB)" -f $i64.LastWriteTime, ($i64.Length / 1MB))

    # RULE A10: the .i64 is only current after a clean close. If any loose working
    # file is newer, the .i64 does NOT contain the latest naming.
    $loose = Get-ChildItem -LiteralPath $idbDir -File -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -like "$idbBase.id0" -or $_.Name -like "$idbBase.id1" -or
                            $_.Name -like "$idbBase.id2" -or $_.Name -like "$idbBase.nam" -or
                            $_.Name -like "$idbBase.til" }
    $newer = $loose | Where-Object { $_.LastWriteTime -gt $i64.LastWriteTime }

    if ($newer) {
        Write-Warn ("the .i64 is STALE -- {0} loose working file(s) are newer, so IDA is open or was killed. " -f $newer.Count +
                    "Save the IDB (Ctrl+W) and re-run, or rely on the loose files copied below.")
        foreach ($f in $newer) { Write-Host ("         newer: {0}  {1}" -f $f.Name, $f.LastWriteTime) -ForegroundColor Yellow }
    } else {
        Write-Ok 'the .i64 is current (no loose working file is newer)'
    }

    $idbDest = Join-Path $Destination "idb\$stamp"
    if ($PSCmdlet.ShouldProcess($IdbPath, "copy to $idbDest")) {
        New-Item -ItemType Directory -Path $idbDest -Force | Out-Null

        # The .i64 itself is not held open by a running IDA (only the loose working
        # files are), so this normally succeeds even mid-session.
        try {
            Copy-Item -LiteralPath $IdbPath -Destination $idbDest -Force -ErrorAction Stop
            Write-Ok "s2x_dump.exe.i64 -> $idbDest"
        } catch {
            Write-Warn "could not copy the .i64: $($_.Exception.Message)"
        }

        # Copy the loose files too: when the .i64 is stale THEY are the database and
        # the only record of the current naming.
        #
        # But IDA holds them open while the database is open, so a copy will fail
        # with a sharing violation. That is NOT an error worth aborting the whole
        # sync for -- it just means IDA is running. Handle each file individually
        # and let the .i64's freshness decide how much it matters.
        $lockedNames = @()
        $copied = 0
        foreach ($f in $loose) {
            try {
                Copy-Item -LiteralPath $f.FullName -Destination $idbDest -Force -ErrorAction Stop
                $copied++
            } catch {
                $lockedNames += $f.Name
            }
        }
        if ($copied) { Write-Ok "$copied loose working file(s) -> $idbDest" }

        if ($lockedNames) {
            if ($newer) {
                # Worst case: the .i64 is behind AND we cannot read the live files.
                # This backup does not contain the newest naming. Say so loudly.
                Write-Warn ("IDA IS OPEN and the .i64 is STALE, so these locked file(s) " +
                            "could not be copied: " + ($lockedNames -join ', ') +
                            ". THIS BACKUP IS MISSING THE LATEST NAMING -- save the IDB " +
                            "(Ctrl+W) in IDA and re-run.")
            } else {
                # Benign: the .i64 is current, so it already contains everything.
                Write-Host ("  [note] IDA is open, so {0} loose file(s) were locked ({1}). " -f $lockedNames.Count, ($lockedNames -join ', ')) -ForegroundColor DarkGray
                Write-Host "         Harmless here: the .i64 is current, so it already has the naming." -ForegroundColor DarkGray
            }
        }
    }
} else {
    Write-Warn "IDB not found at $IdbPath"
}

# --------------------------------------------------------------- demos ----
Write-Section 'Recordings (cannot be regenerated)'
Invoke-Robocopy -Source (Join-Path $GamePath 'main\demo')  -Dest (Join-Path $Destination 'demos\native') -Files @('*.demo')
Invoke-Robocopy -Source (Join-Path $GamePath 'demos')      -Dest (Join-Path $Destination 'demos\dm_s2')  -Files @('*.dm_s2')
Invoke-Robocopy -Source (Join-Path $GamePath 'players2\demo') -Dest (Join-Path $Destination 'demos\players2') -Files @('*.demo')

# ---------------------------------------------------- logs and config ----
Write-Section 'Logs and config'
$singles = @(
    @{ p = (Join-Path $GamePath 'main\s2mp_console.log'); n = 'console log' },
    @{ p = (Join-Path $GamePath 's2mp_bans.txt');         n = 'ban list'    },
    @{ p = (Join-Path $GamePath 's2mp-mod.cfg');          n = 'mod config'  }
)
$logDest = Join-Path $Destination "logs\$stamp"
foreach ($s in $singles) {
    if (Test-Path -LiteralPath $s.p) {
        if ($PSCmdlet.ShouldProcess($s.p, "copy to $logDest")) {
            New-Item -ItemType Directory -Path $logDest -Force | Out-Null
            Copy-Item -LiteralPath $s.p -Destination $logDest -Force
            Write-Ok "$($s.n)  ->  $logDest"
        }
    } else { Write-Skip "$($s.n) ($($s.p))" }
}

# Crash dumps: RULE A13 says read these first after any crash, and they accumulate
# into a changelog of which fix moved which crash. Worth keeping.
Invoke-Robocopy -Source $GamePath -Dest (Join-Path $Destination 'crashdumps') -Files @('*.dmp') -Extra @('/LEV:1')

# -------------------------------------------------------------- result ----
Write-Section 'Result'
if ($script:Problems.Count -eq 0) {
    Write-Host "  Everything copied cleanly." -ForegroundColor Green
} else {
    Write-Host "  $($script:Problems.Count) thing(s) need your attention:" -ForegroundColor Yellow
    foreach ($p in $script:Problems) { Write-Host "    - $p" -ForegroundColor Yellow }
}
Write-Host "  Destination: $Destination`n"

# Exit code discipline. robocopy sets $LASTEXITCODE to 1 for "files were copied",
# which is SUCCESS in robocopy's scheme but would make a scheduled task or a CI
# runner report this script as failed. Decide the exit code ourselves: 0 unless
# something genuinely could not be saved.
if ($script:Problems.Count -gt 0) { exit 2 }
exit 0
