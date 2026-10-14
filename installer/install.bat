<# :
@echo off
setlocal
REM ==========================================================================
REM  Dynamic Flags Mechanism - installer for the flag DLL
REM
REM  Double-click this file. It can stay where it is: it finds your Europa
REM  Universalis IV folder by itself (or asks you for it).
REM
REM  It copies winmm.dll and cpbm_identity.dll into the game folder, makes
REM  winmm_orig.dll from your own Windows copy, then checks a mod's flag rules.
REM  Read README.txt next to this file for the details.
REM ==========================================================================
REM
REM  MODDERS - THIS IS THE ONE LINE TO EDIT
REM
REM  TARGET_MOD is the mod whose flag rules this installer checks after the
REM  DLL is in place. It does NOT change what the DLL does: the DLL acts on
REM  flag lines from EVERY enabled mod, whatever is written here.
REM
REM    set "TARGET_MOD="            ask the player which mod to check (or skip)
REM    set "TARGET_MOD=MyMod"       a mod folder name, or its name= from the .mod
REM    set "TARGET_MOD=123456789"   a Steam Workshop id - best for Workshop mods,
REM                                 because subscribers' folders are named by id
REM
REM  When you ship this folder inside your own mod, set it to your mod.
REM ==========================================================================
set "TARGET_MOD="

set "DFM_SELF=%~f0"
set "DFM_EU4=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command "iex ([IO.File]::ReadAllText($env:DFM_SELF))"
echo.
pause
exit /b
#>

# ---------------------------------------------------------------------------
# Everything below is PowerShell, run by the three lines above. Keeping it in
# the .bat means there is one file to double-click and nothing to unblock.
# Test without writing anything:  set DFM_DRYRUN=1  then run install.bat
# ---------------------------------------------------------------------------

$PinnedHash  = '9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A'
$PinnedBuild = '1.37.5.0 Inca (491d)'
$DllSha      = '070801A60DEE9E76F7E746C8092147B17E44C0A11AEBC26ABD6E38978E71BA50'

$Target  = $env:TARGET_MOD
$Here    = (Split-Path -Parent $env:DFM_SELF).TrimEnd('\')
$DryRun  = [bool]$env:DFM_DRYRUN
$Answers = $null
if ($env:DFM_ANSWERS) { $Answers = New-Object System.Collections.Queue (,[object[]]($env:DFM_ANSWERS -split '\|')) }

function Say([string]$t = '') { Write-Host $t }
function Good([string]$t)     { Write-Host "  + $t" -ForegroundColor Green }
function Warn([string]$t)     { Write-Host "  ! $t" -ForegroundColor Yellow }
function Info([string]$t)     { Write-Host "    $t" }

function Ask([string]$prompt) {
    if ($Answers) {
        $a = ''
        if ($Answers.Count) { $a = [string]$Answers.Dequeue() }
        Write-Host "$prompt $a"
        return $a
    }
    return (Read-Host $prompt)
}

function Test-Eu4([string]$d) { try { $d -and [IO.File]::Exists([IO.Path]::Combine($d, 'eu4.exe')) } catch { $false } }

function Find-Eu4 {
    $c = New-Object System.Collections.Generic.List[string]
    if ($env:DFM_EU4) { $c.Add($env:DFM_EU4) }
    $c.Add($Here)                                          # copied into the game folder, the old way
    if ($Here -match '^(.*\\steamapps)\\') {                # run from a Workshop download
        $c.Add([IO.Path]::Combine($Matches[1], 'common\Europa Universalis IV'))
    }
    $steam = @()
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty -LiteralPath $k -ErrorAction Stop
            foreach ($n in 'SteamPath', 'InstallPath') { if ($p.$n) { $steam += ($p.$n -replace '/', '\') } }
        } catch {}
    }
    $steam += [IO.Path]::Combine(${env:ProgramFiles(x86)}, 'Steam')
    foreach ($s in ($steam | Select-Object -Unique)) {
        $c.Add([IO.Path]::Combine($s, 'steamapps\common\Europa Universalis IV'))
        $vdf = [IO.Path]::Combine($s, 'steamapps\libraryfolders.vdf')
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"]+)"')) {
                $c.Add([IO.Path]::Combine(($m.Groups[1].Value -replace '\\\\', '\'), 'steamapps\common\Europa Universalis IV'))
            }
        }
    }
    foreach ($d in $c) { if (Test-Eu4 $d) { return (Resolve-Path -LiteralPath $d).ProviderPath.TrimEnd('\') } }
    return $null
}

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-Writable([string]$d) {
    try {
        $t = Join-Path $d ('.dfm_write_test_' + [guid]::NewGuid().ToString('N'))
        [IO.File]::WriteAllText($t, 'x')
        Remove-Item -LiteralPath $t -Force
        return $true
    } catch { return $false }
}

function Copy-One([string]$from, [string]$to) {
    if ($from -ieq $to) { Info "$(Split-Path -Leaf $to) already in place"; return }
    if ($DryRun) { Info "(dry run) would copy $from -> $to"; return }
    Copy-Item -LiteralPath $from -Destination $to -Force
    Good "copied $(Split-Path -Leaf $to)"
}

# --- reading .mod files ------------------------------------------------------

function Read-ModFile($file, [string]$userDir) {
    $text = [IO.File]::ReadAllText($file.FullName)
    $o = [ordered]@{ File = $file; Name = $file.BaseName; Path = $null; Id = $null; Folder = $null }
    if ($text -match '(?m)^\s*name\s*=\s*"([^"]*)"')          { $o.Name = $Matches[1] }
    if ($text -match '(?m)^\s*path\s*=\s*"([^"]*)"')          { $o.Path = $Matches[1] }
    if ($text -match '(?m)^\s*remote_file_id\s*=\s*"?(\d+)')  { $o.Id   = $Matches[1] }
    if ($o.Path) {
        $p = $o.Path -replace '/', '\'
        if (-not [IO.Path]::IsPathRooted($p)) { $p = Join-Path $userDir $p }
        $o.Folder = $p.TrimEnd('\')
    }
    [pscustomobject]$o
}

function Test-WorkshopFolder([string]$f) { $f -match '\\steamapps\\workshop\\' }

# Every .tga the DLL could find, the way it looks: each .mod's path, then the game.
function Get-KnownArt($mods, [string]$eu4) {
    $set = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $dirs = @()
    foreach ($m in $mods) {
        if ($m.Folder) { $dirs += [IO.Path]::Combine($m.Folder, 'gfx\flags') }
    }
    $dirs += (Join-Path $eu4 'gfx\flags')
    foreach ($d in ($dirs | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $d) {
            foreach ($f in [IO.Directory]::EnumerateFiles($d, '*.tga')) { [void]$set.Add([IO.Path]::GetFileNameWithoutExtension($f)) }
        }
    }
    return ,$set
}

function Add-StarterFiles($mod) {
    $prefix = ((Split-Path -Leaf $mod.Folder).ToLower() -replace '[^a-z0-9]+', '_').Trim('_')
    if (-not $prefix -or $prefix -match '^\d') { $prefix = "mod_$prefix".TrimEnd('_') }

    $effects = @'
# __PREFIX__ - dynamic flag rules, read by the flag DLL.
#
#   log = "dynamic_country_flag <TAG> <flagname>"   show gfx/flags/<flagname>.tga as TAG's flag
#   log = "dynamic_country_flag <TAG> -"            give TAG its own flag back
#
# Write the tag out in the log line, always pair an if with an else that logs "-",
# and give your flag files a prefix of your own. See the Dynamic Flags Mechanism
# README for the full rules.

__PREFIX___dynamic_flags_evaluate = {

	# Example - remove the # marks and change the tag, trigger and flag name:
	#
	# if = {
	# 	limit = { tag = ENG }
	# 	if = {
	# 		limit = { government = republic }
	# 		log = "dynamic_country_flag ENG MYMOD_ENG_commonwealth"
	# 	}
	# 	else = {
	# 		log = "dynamic_country_flag ENG -"
	# 	}
	# }
}
'@
    $onActions = @'
# __PREFIX__ - runs the dynamic flag rules. A uniquely named file, so it adds to
# vanilla's and other mods' on_actions instead of replacing them.

on_startup = {
	__PREFIX___dynamic_flags_evaluate = yes
}

on_monthly_pulse = {
	__PREFIX___dynamic_flags_evaluate = yes
}
'@
    $plan = @(
        @{ Rel = "common\scripted_effects\${prefix}_dynamic_flags_effects.txt"; Text = $effects },
        @{ Rel = "common\on_actions\${prefix}_dynamic_flags_on_actions.txt";    Text = $onActions }
    )
    foreach ($p in $plan) {
        $dest = Join-Path $mod.Folder $p.Rel
        if (Test-Path -LiteralPath $dest) { Warn "$($p.Rel) already exists - left alone"; continue }
        if ($DryRun) { Info "(dry run) would create $($p.Rel)"; continue }
        [void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest))
        [IO.File]::WriteAllText($dest, ($p.Text -replace '__PREFIX__', $prefix), (New-Object Text.UTF8Encoding $false))
        Good "created $($p.Rel)"
    }
    $flags = Join-Path $mod.Folder 'gfx\flags'
    if (-not (Test-Path -LiteralPath $flags) -and -not $DryRun) { [void](New-Item -ItemType Directory -Force -Path $flags) }
    Info "Put your flag art (128x128 .tga) in $flags"
}

function Test-Mod($mod, $mods, $enabled, [string]$eu4, [bool]$offerStarter) {
    Say ''
    Say "--- $($mod.Name)   [$($mod.File.Name)]"
    if (-not $mod.Folder -or -not (Test-Path -LiteralPath $mod.Folder)) {
        Warn "its folder does not exist: $($mod.Folder)"
        return
    }
    Info "folder: $($mod.Folder)"

    if ($enabled -contains ('mod/' + $mod.File.Name)) { Good 'enabled in the launcher' }
    else { Warn 'not enabled in the launcher at the moment - its rules only run when it is' }


    $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $files = 0
    foreach ($sub in 'common', 'events', 'decisions', 'missions') {
        $dir = Join-Path $mod.Folder $sub
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        foreach ($f in [IO.Directory]::EnumerateFiles($dir, '*.txt', 'AllDirectories')) {
            $hits = [regex]::Matches([IO.File]::ReadAllText($f), '(?m)^[^#\r\n]*?(?:dynamic_country_flag|cpbm_flag)\s+\S{1,7}\s+([^\s"#]+)')
            if ($hits.Count) { $files++; foreach ($h in $hits) { [void]$names.Add($h.Groups[1].Value) } }
        }
    }

    if ($files -eq 0) {
        Warn 'no dynamic_country_flag lines found in common, events, decisions or missions'
        if ($offerStarter -and -not (Test-WorkshopFolder $mod.Folder)) {
            if ((Ask '    Add starter rule files to this mod? (y/N)') -match '^[yY]') { Add-StarterFiles $mod }
        }
        return
    }
    [void]$names.Remove('-')
    Good "$($names.Count) flag name(s) used, in $files file(s)"

    # The DLL only looks in enabled mods - plus this one, which will be enabled to play.
    $artMods = $mods
    if ($enabled.Count) { $artMods = @($mods | Where-Object { ($enabled -contains ('mod/' + $_.File.Name)) -or $_.File.FullName -eq $mod.File.FullName }) }
    $art = Get-KnownArt $artMods $eu4
    $missing = @($names | Where-Object { -not $art.Contains($_) } | Sort-Object)
    $long    = @($names | Where-Object { $_.Length -gt 31 })
    if ($missing.Count) {
        Warn "$($missing.Count) of them have no .tga in any enabled mod's gfx\flags (nothing will be painted for those):"
        Info (($missing | Select-Object -First 12) -join ', ')
        if ($missing.Count -gt 12) { Info "... and $($missing.Count - 12) more" }
    } else { Good 'every one of them has a .tga the DLL can find' }
    if ($long.Count) { Warn "flag names longer than 31 characters get cut short by the DLL: $($long -join ', ')" }
}

# --- the install ---------------------------------------------------------------

function Main {
    Say '=== Dynamic Flags Mechanism - flag DLL installer ==='
    if ($DryRun) { Warn 'DRY RUN - nothing will be written' }
    Say ''

    foreach ($f in 'winmm.dll', 'cpbm_identity.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $Here $f))) {
            Warn "$f is missing from $Here - keep it next to install.bat."
            return
        }
    }

    $eu4 = Find-Eu4
    while (-not $eu4) {
        Warn 'Could not find Europa Universalis IV by itself.'
        $a = (Ask '    Paste the folder that contains eu4.exe (Enter to quit):').Trim().Trim('"')
        if (-not $a) { return }
        if (Test-Eu4 $a) { $eu4 = $a.TrimEnd('\') } else { Warn "no eu4.exe in $a" }
    }
    Good "game folder: $eu4"

    if (Get-Process -Name eu4 -ErrorAction SilentlyContinue) {
        Warn 'EU4 is running. Close the game, then run this again.'
        return
    }

    $exeHash = (Get-FileHash -LiteralPath (Join-Path $eu4 'eu4.exe') -Algorithm SHA256).Hash
    if ($exeHash -eq $PinnedHash) { Good "eu4.exe is $PinnedBuild - supported" }
    else {
        Warn "eu4.exe is not $PinnedBuild, the version this DLL was made for."
        Info 'It will refuse to attach: the game runs normally, the flags just never change.'
    }

    if ((Get-FileHash -LiteralPath (Join-Path $Here 'cpbm_identity.dll') -Algorithm SHA256).Hash -ne $DllSha) {
        Info 'note: this cpbm_identity.dll is a different build from the one this installer was written for.'
    }

    if (-not (Test-Writable $eu4)) {
        if ($DryRun) { Warn 'the game folder needs administrator rights to write (dry run: not asking)' }
        elseif (Test-Admin) { Warn "Cannot write to $eu4, even as administrator."; return }
        else {
            Say '  Windows needs administrator rights to write into the game folder - asking now.'
            try {
                Start-Process -FilePath $env:DFM_SELF -ArgumentList ('"' + $eu4 + '"') -Verb RunAs
                Info 'Carry on in the new window.'
            } catch { Warn 'Not allowed. Right-click install.bat and choose "Run as administrator".' }
            return
        }
    }

    Say ''
    $shim = Join-Path $eu4 'winmm.dll'
    if ($Here -ine $eu4 -and (Test-Path -LiteralPath $shim) -and
        (Get-FileHash -LiteralPath $shim).Hash -ne (Get-FileHash -LiteralPath (Join-Path $Here 'winmm.dll')).Hash) {
        Warn 'a different winmm.dll is already in the game folder (an older copy of this, or another tool).'
        if ($DryRun) { Info '(dry run) would keep it as winmm.dll.bak' }
        else { Copy-Item -LiteralPath $shim -Destination "$shim.bak" -Force; Info 'kept it as winmm.dll.bak' }
    }
    Copy-One (Join-Path $Here 'winmm.dll')         $shim
    Copy-One (Join-Path $Here 'cpbm_identity.dll') (Join-Path $eu4 'cpbm_identity.dll')
    $sysWinmm = if ([Environment]::Is64BitProcess) { "$env:SystemRoot\System32\winmm.dll" } else { "$env:SystemRoot\Sysnative\winmm.dll" }
    Copy-One $sysWinmm (Join-Path $eu4 'winmm_orig.dll')

    # --- which mod's rules to check ---
    $userDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Paradox Interactive\Europa Universalis IV'
    $modDir  = Join-Path $userDir 'mod'
    Say ''
    if (-not (Test-Path -LiteralPath $modDir)) {
        Warn "no mod folder at $modDir yet - start EU4 once, then run this again to check your mod."
        return
    }
    $mods = @(Get-ChildItem -LiteralPath $modDir -Filter *.mod -File | ForEach-Object { Read-ModFile $_ $userDir })
    $enabled = @()
    try { $enabled = @(([IO.File]::ReadAllText((Join-Path $userDir 'dlc_load.json')) | ConvertFrom-Json).enabled_mods) } catch {}

    $chosen = @()
    if ($Target) {
        $chosen = @($mods | Where-Object {
            $_.Id -eq $Target -or $_.File.BaseName -eq $Target -or $_.File.BaseName -eq "ugc_$Target" -or
            $_.Name -eq $Target -or ($_.Folder -and (Split-Path -Leaf $_.Folder) -eq $Target) })
        # A local copy and a Workshop copy can both match; check the enabled one(s) if any are.
        $on = @($chosen | Where-Object { $enabled -contains ('mod/' + $_.File.Name) })
        if ($on.Count) { $chosen = $on }
        if (-not $chosen.Count) {
            Warn "This installer is set up for the mod '$Target', which is not installed."
            Info 'Subscribe to it (or copy it into your mod folder) and enable it in the launcher.'
            Info 'The DLL itself is installed either way.'
        }
    } else {
        Say 'The DLL reads flag rules from every enabled mod at once. You can now check'
        Say 'one mod: that its rules are found and every flag it names has art.'
        Say ''
        $list = @($mods | Where-Object { $_.Folder -and (Test-Path -LiteralPath $_.Folder) } | Sort-Object Name, @{ e = { $_.File.Name } })
        for ($i = 0; $i -lt $list.Count; $i++) {
            $mark = if ($enabled -contains ('mod/' + $list[$i].File.Name)) { ' [enabled]' } else { '' }
            Say ('  {0,3}. {1}   ({2}){3}' -f ($i + 1), $list[$i].Name, $list[$i].File.Name, $mark)
        }
        Say ''
        $a = Ask 'Number of the mod to check (Enter to skip):'
        $n = 0
        if ([int]::TryParse($a, [ref]$n) -and $n -ge 1 -and $n -le $list.Count) { $chosen = @($list[$n - 1]) }
    }
    foreach ($m in $chosen) { Test-Mod $m $mods $enabled $eu4 (-not $Target) }

    Say ''
    Say '=== Done ==='
    Info 'Start EU4 from the launcher with your mods enabled.'
    Info "What the DLL did each launch: $eu4\cpbm-identity.log"
    Info 'To uninstall: delete winmm.dll, winmm_orig.dll and cpbm_identity.dll from the game folder.'
    if (Test-Path -LiteralPath "$shim.bak") { Info '(then rename winmm.dll.bak back to winmm.dll)' }
}

try { Main } catch { Warn "Stopped by an error: $($_.Exception.Message)" }
