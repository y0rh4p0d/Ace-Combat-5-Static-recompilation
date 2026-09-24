# Turn this working copy into a commit on your own GitHub fork.
#
#   pwsh -File tools/fork_and_commit.ps1 -User <your-github-name>
#   pwsh -File tools/fork_and_commit.ps1 -User <name> -DryRun     # show the plan only
#   pwsh -File tools/fork_and_commit.ps1 -User <name> -Message "..." 
#
# GitHub does not let a script create a fork without a token, and this machine
# reaches github.com over SSH only (DNS for the web/API side is pinned to a dead
# IP in ~/.ssh/config), so the fork has to be made once in the browser.  This
# script does everything else: configures the remotes, checks the staged set,
# commits, and pushes.
#
# Make the fork at:
#   https://github.com/sal063/Ace-Combat-5-Static-recompilation/fork

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$User,                       # your GitHub account, e.g. y0rh4p0d

    [string]$Upstream = 'sal063/Ace-Combat-5-Static-recompilation',
    [string]$Branch   = 'main',
    [string]$Message  = '',

    [switch]$DryRun,
    [switch]$NoPush
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
Push-Location $root
try {
    $forkSsh   = "git@github.com:$User/Ace-Combat-5-Static-recompilation.git"
    $forkHttps = "https://github.com/$User/Ace-Combat-5-Static-recompilation.git"
    $upSsh     = "git@github.com:$Upstream.git"

    # Name the executable explicitly, not `git`: a function called Git shadowing
    # the real command makes `& git` call the function again and recurse forever.
    $GIT = (Get-Command git.exe -ErrorAction SilentlyContinue).Source
    if (-not $GIT) { $GIT = (Get-Command git -ErrorAction Stop).Source }

    function Run-Git([string[]]$a) { & $GIT @a; if ($LASTEXITCODE -ne 0) { throw "git $($a -join ' ') failed" } }

    Write-Host "== fork: $User/Ace-Combat-5-Static-recompilation" -ForegroundColor Cyan

    # --- 1. is the fork reachable? ----------------------------------------
    Write-Host "-- checking the fork exists"
    $env:GIT_SSH_COMMAND = 'ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15'
    $null = & $GIT ls-remote $forkSsh HEAD 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "Cannot reach $forkSsh" -ForegroundColor Red
        Write-Host ""
        Write-Host "Create the fork first, in a browser:" -ForegroundColor Yellow
        Write-Host "  https://github.com/$Upstream/fork"
        Write-Host ""
        Write-Host "Forking cannot be automated here: github.com's web/API side is not"
        Write-Host "reachable from this machine (its DNS is pinned to a dead address in"
        Write-Host "~/.ssh/config), and creating a fork needs an API token."
        exit 1
    }
    Write-Host "   fork is reachable" -ForegroundColor Green

    # --- 2. remotes -------------------------------------------------------
    Write-Host "-- configuring remotes"
    $have = (& $GIT remote) -split "`n" | ForEach-Object { $_.Trim() }
    if ($have -contains 'fork') { Run-Git @('remote', 'set-url', 'fork', $forkSsh) }
    else { Run-Git @('remote', 'add', 'fork', $forkSsh) }
    if ($have -contains 'upstream') { Run-Git @('remote', 'set-url', 'upstream', $upSsh) }
    else { Run-Git @('remote', 'add', 'upstream', $upSsh) }
    Write-Host "   origin   -> $((& $GIT remote get-url origin))"
    Write-Host "   fork     -> $forkSsh"
    Write-Host "   upstream -> $upSsh"

    # --- 3. what is about to be committed ---------------------------------
    Write-Host "-- staging everything"
    Run-Git @('add', '-A')

    $stat = & $GIT diff --cached --stat | Select-Object -Last 1
    $files = (& $GIT diff --cached --name-only | Measure-Object).Count
    if ($files -eq 0) {
        Write-Host "   nothing to commit; the tree already matches HEAD" -ForegroundColor Yellow
    } else {
        Write-Host "   $files file(s): $stat"
    }

    # Anything enormous is almost always a mistake; these are all ignored, but say
    # so loudly rather than letting a 500 MB blob into the history.
    Write-Host "-- checking for oversized files"
    $big = & $GIT diff --cached --name-only |
        Where-Object { $_ } |
        ForEach-Object { $f = Join-Path $root $_; if (Test-Path $f) { [pscustomobject]@{ f = $_; mb = (Get-Item $f).Length / 1MB } } } |
        Where-Object { $_.mb -gt 20 } | Sort-Object mb -Descending
    if ($big) {
        Write-Host "   these are over 20 MB:" -ForegroundColor Red
        $big | ForEach-Object { Write-Host ("     {0,8:N1} MB  {1}" -f $_.mb, $_.f) }
        Write-Host "   (the SDL3 DLLs are ~4 MB each; anything much larger is suspect)"
    } else {
        Write-Host "   none over 20 MB" -ForegroundColor Green
    }

    # --- 4. identity ------------------------------------------------------
    $name  = & $GIT config user.name
    $email = & $GIT config user.email
    if (-not $name -or -not $email) {
        Write-Host "-- no commit identity set; setting one from your GitHub account"
        Run-Git @('config', 'user.name', $User)
        Run-Git @('config', 'user.email', "$User@users.noreply.github.com")
        $name = $User
        $email = "$User@users.noreply.github.com"
    }
    Write-Host "   author: $name <$email>"

    if ($DryRun) {
        Write-Host ""
        Write-Host "-DryRun: nothing committed. The plan above is what would happen." -ForegroundColor Yellow
        exit 0
    }

    # --- 5. commit --------------------------------------------------------
    if ($files -gt 0) {
        if (-not $Message) {
            $Message = @"
Add the Japanese / Chinese region and a Windows-on-ARM build

Recompile either release from its own executable.  The Japanese build is a
different program from the US one, so tools/cnjp translates the US config into
config/cnjp mechanically and reproducibly instead of by hand.

Two region-dependent addresses in the runtime are now found at run time rather
than compiled in:

  * the native renderer's tap points and the render-intent layer, which resolve
    by scanning the loaded executable.  The intents fall into five shift regions
    and are resolved a region at a time, because their frame prologues are far
    too common to identify individually.

  * the sound-bank transfer counter the scene state machine waits on.  It is at
    0x0047EC9C on the US build and 0x0047F49C on the Japanese one; reading the US
    address on the Japanese build returns a constant zero, which left the game
    waiting forever for a transfer that had already completed.  That was the
    black screen: the build booted, loaded the disc, attached every hook and
    reported no errors, but never left its first screen.

Also:

  * the build copies SDL3 and pthread next to ac5.exe, per architecture, so the
    exe runs from anywhere without a hand-copied DLL.
  * --data now searches the usual output directories and, when it fails, prints
    every path it tried.
  * -fpatchable-function-entry is disabled for clang on Windows, where it
    conflicts with clang's SEH prologue data and breaks the AArch64 build.
  * tools/build_all.ps1 builds any or all of the three targets in one command.
"@
        }
        Run-Git @('commit', '-m', $Message)
        Write-Host "   committed" -ForegroundColor Green
    }

    # --- 6. push ----------------------------------------------------------
    if ($NoPush) { Write-Host "-- -NoPush: stopping before the push"; exit 0 }

    Write-Host "-- pushing to fork/$Branch"
    & $GIT push fork "$Branch"
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "Push failed. If the fork is empty this is usually because the remote" -ForegroundColor Yellow
        Write-Host "branch does not exist yet; retry with:" -ForegroundColor Yellow
        Write-Host "  git push -u fork $Branch"
        exit 1
    }

    Write-Host ""
    Write-Host "done. Your fork:" -ForegroundColor Green
    Write-Host "  $forkHttps"
    Write-Host ""
    Write-Host "To open a pull request against the original:"
    Write-Host "  $forkHttps/compare/$Branch?expand=1"
}
finally {
    Pop-Location
}
