#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName = 'Capture')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Capture')]
    [string] $ExePath,
    [Parameter(Mandatory, ParameterSetName = 'Capture')]
    [string] $SymbolsDirectory,
    [Parameter(ParameterSetName = 'Capture')]
    [string] $OutputDirectory = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Telegram-memory'),
    [Parameter(ParameterSetName = 'Capture')]
    [ValidateRange(60, 900)]
    [int] $MaxSeconds = 600,
    [Parameter(ParameterSetName = 'Capture')]
    [switch] $CheckOnly,
    [Parameter(Mandatory, ParameterSetName = 'Recover')]
    [string] $Recover
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$script:IfeoPath = 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\Telegram.exe'

function Write-JsonFile($Path, $Value) {
    $temporary = $Path + '.tmp'
    [IO.File]::WriteAllText($temporary, (ConvertTo-Json -InputObject $Value -Depth 10), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Assert-CaptureState($State) {
    if ($State.schema_version -ne 1 -or $State.instance -cnotmatch '^TdMemory[0-9a-f]{32}$') {
        throw 'Invalid recovery file schema or recording instance.'
    }
    foreach ($key in @('original_exists', 'tracing_armed', 'start_attempted')) {
        if ($State[$key] -isnot [bool]) { throw "Invalid recovery field: $key" }
    }
    if ($State.original_flags -isnot [long] -and $State.original_flags -isnot [int]) {
        throw 'Invalid original TracingFlags.'
    }
    if ($State.original_flags -lt 0 -or $State.original_flags -gt [uint32]::MaxValue) {
        throw 'TracingFlags is outside the DWORD range.'
    }
    if (-not $State.original_exists -and $State.original_flags -ne 0) {
        throw 'An absent TracingFlags value must have a zero fallback.'
    }
    if (($State.original_flags -band 1L) -ne 0) {
        throw 'This collector cannot own a pre-existing heap tracing configuration.'
    }
}

function Get-TracingSetting {
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        $key = $registry.OpenSubKey($script:IfeoPath)
        try {
            if ($null -eq $key) { return @{ exists = $false; flags = [long]0 } }
            if ($key.GetValue('UseFilter', 0) -ne 0) {
                throw 'IFEO path filtering is configured. Resolve that tracing configuration first.'
            }
            $exists = $key.GetValueNames() -contains 'TracingFlags'
            if (-not $exists) { return @{ exists = $false; flags = [long]0 } }
            if ($key.GetValueKind('TracingFlags') -ne [Microsoft.Win32.RegistryValueKind]::DWord) {
                throw 'Existing TracingFlags is not a DWORD; it will not be changed.'
            }
            return @{ exists = $true; flags = ([long]$key.GetValue('TracingFlags') -band 0xffffffffL) }
        } finally {
            if ($null -ne $key) { $key.Dispose() }
        }
    } finally {
        $registry.Dispose()
    }
}

function Set-TracingSetting($Exists, [long] $Flags) {
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        $key = if ($Exists) { $registry.CreateSubKey($script:IfeoPath) } else { $registry.OpenSubKey($script:IfeoPath, $true) }
        try {
            if ($Exists) {
                $signed = [BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]$Flags), 0)
                $key.SetValue('TracingFlags', $signed, [Microsoft.Win32.RegistryValueKind]::DWord)
            } elseif ($null -ne $key) {
                $key.DeleteValue('TracingFlags', $false)
            }
        } finally {
            if ($null -ne $key) { $key.Dispose() }
        }
    } finally {
        $registry.Dispose()
    }
}

function Get-RestoreAction($State, $Current) {
    if ($Current.exists -eq $State.original_exists -and $Current.flags -eq $State.original_flags) {
        return 'None'
    }
    if ($Current.exists -and $Current.flags -eq ($State.original_flags -bor 1L)) {
        return 'Restore'
    }
    throw 'TracingFlags changed outside this capture. Refusing to overwrite the newer setting.'
}

function Enter-CaptureLock {
    $mutex = [Threading.Mutex]::new($false, 'Global\TelegramMemoryCapture')
    try {
        try { $acquired = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $acquired = $true }
        if (-not $acquired) { throw 'Another memory capture or recovery is running.' }
        return $mutex
    } catch {
        $mutex.Dispose()
        throw
    }
}

function Invoke-Wpr([string[]] $Arguments, [switch] $AllowFailure) {
    $output = & $script:WprPath @Arguments 2>&1
    $code = $LASTEXITCODE
    $text = $output -join [Environment]::NewLine
    Add-Content -LiteralPath $script:WprLog -Value ("{0:o} wpr {1}`n{2}`nexit={3}" -f [DateTimeOffset]::Now, ($Arguments -join ' '), $text, $code) -Encoding utf8
    if ($code -ne 0 -and -not $AllowFailure) { throw "WPR failed ($code): $text" }
    return @{ exit_code = $code; output = $text }
}

function Restore-Capture($State, $StatePath) {
    Assert-CaptureState $State
    $failures = [Collections.Generic.List[string]]::new()
    if ($State.tracing_armed) {
        try {
            $action = Get-RestoreAction $State (Get-TracingSetting)
            if ($action -eq 'Restore') {
                Set-TracingSetting $State.original_exists $State.original_flags
            }
            $State.tracing_armed = $false
            Write-JsonFile $StatePath $State
        } catch {
            $failures.Add($_.Exception.Message)
        }
    }
    if ($State.start_attempted) {
        try {
            $result = Invoke-Wpr @('-cancel', '-instancename', $State.instance) -AllowFailure
            if ($result.exit_code -ne 0 -and $result.output -notmatch '\b0xc5583000\b') {
                throw "Could not confirm cancellation of $($State.instance). Check wpr.log and WPR -status for this instance."
            }
            $State.start_attempted = $false
            Write-JsonFile $StatePath $State
        } catch {
            $failures.Add($_.Exception.Message)
        }
    }
    if ($failures.Count) { throw ($failures -join [Environment]::NewLine) }
}

function Assert-BuildPair($Executable, $Symbols) {
    $manifestPath = Join-Path $Symbols 'build-info.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
    if ($manifest.schema_version -ne 1 -or $manifest.architecture -ne 'x64') {
        throw 'Expected the x64 build-info.json from the same GitHub Actions run.'
    }
    foreach ($name in @('Telegram.exe', 'Telegram.pdb')) {
        $path = if ($name -eq 'Telegram.exe') { $Executable } else { Join-Path $Symbols $name }
        $expected = $manifest.files[$name]
        if ($null -eq $expected -or $expected.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Missing or invalid SHA256 for $name in build-info.json."
        }
        $file = Get-Item -LiteralPath $path
        if ($file.Length -ne $expected.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected.sha256) {
            throw "$name does not match build-info.json. Download both artifacts from the same Actions run."
        }
    }
    return $manifest
}

function Get-ClientProcesses {
    return @(Get-Process -Name Telegram -ErrorAction SilentlyContinue)
}

function Assert-NormalPrivilege([int] $ProcessId) {
    if (-not ('TdMemory.ProcessToken' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace TdMemory {
    public static class ProcessToken {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int id);
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool GetTokenInformation(IntPtr token, int info, out int value, int size, out int returned);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);
        public static bool IsElevated(int id) {
            var process = OpenProcess(0x1000, false, id);
            if (process == IntPtr.Zero) throw new Win32Exception();
            try {
                IntPtr token;
                if (!OpenProcessToken(process, 8, out token)) throw new Win32Exception();
                try {
                    int elevated, returned;
                    if (!GetTokenInformation(token, 20, out elevated, 4, out returned)) throw new Win32Exception();
                    return elevated != 0;
                } finally { CloseHandle(token); }
            } finally { CloseHandle(process); }
        }
    }
}
'@
    }
    if ([TdMemory.ProcessToken]::IsElevated($ProcessId)) {
        throw 'Telegram is elevated. Quit it normally and repeat with Telegram launched from a normal desktop.'
    }
}

function Assert-TargetProcess {
    $processes = @(Get-ClientProcesses)
    if ($processes.Count -ne 1 -or $processes[0].Id -ne $script:Target.Id) {
        throw 'The target exited or another Telegram.exe started. This recording cannot represent one process.'
    }
    $process = $processes[0]
    if ($process.Path -ne $script:Executable -or $process.StartTime.ToUniversalTime() -ne $script:TargetStarted) {
        throw 'The target path or process start time changed.'
    }
    return $process
}

function Assert-RecordingBudget {
    if ([DateTime]::UtcNow -ge $script:Deadline) { throw 'Capture time limit reached.' }
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($script:CaptureDirectory))
    if ($drive.AvailableFreeSpace -lt 2GB) { throw 'Less than 2 GiB free; stopping the capture.' }
    if ($null -ne $script:Target) { $null = Assert-TargetProcess }
}

function Wait-Stage([string] $Message) {
    Write-Host $Message -ForegroundColor Cyan
    Write-Host 'Press Enter when ready, or Q to cancel and restore tracing.'
    while ($true) {
        Assert-RecordingBudget
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq [ConsoleKey]::Q) { throw 'Capture cancelled by the operator.' }
            if ($key.Key -eq [ConsoleKey]::Enter) { return }
        }
        Start-Sleep -Milliseconds 250
    }
}

function Save-Stage([string] $Name) {
    Assert-RecordingBudget
    $process = Assert-TargetProcess
    $null = Invoke-Wpr @('-marker', "TdMemory:$Name PID=$($process.Id)", '-instancename', $script:State.instance)
    $script:Stages.Add([ordered]@{
        stage = $Name
        utc = [DateTime]::UtcNow.ToString('o')
        pid = $process.Id
        start_utc = $script:TargetStarted.ToString('o')
        private_bytes = $process.PrivateMemorySize64
        working_set_bytes = $process.WorkingSet64
    })
    Write-JsonFile (Join-Path $script:CaptureDirectory 'stages.json') $script:Stages.ToArray()
}

if (-not $IsWindows -or -not [Environment]::Is64BitProcess) {
    throw 'Use 64-bit PowerShell 7 on Windows.'
}
$script:WprPath = Join-Path $env:SystemRoot 'System32\wpr.exe'
if (-not (Test-Path -LiteralPath $script:WprPath -PathType Leaf)) { throw 'Windows Performance Recorder is unavailable.' }
if (-not $CheckOnly) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw 'Run this capture script in an administrator PowerShell 7. Keep Telegram itself at normal privilege.'
        }
    } finally {
        $identity.Dispose()
    }
}
if ($PSCmdlet.ParameterSetName -eq 'Recover') {
    $statePath = (Resolve-Path -LiteralPath $Recover).ProviderPath
    $script:WprLog = Join-Path (Split-Path -Parent $statePath) 'wpr.log'
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json -AsHashtable
    $captureLock = Enter-CaptureLock
    try {
        Restore-Capture $state $statePath
    } finally {
        $captureLock.ReleaseMutex()
        $captureLock.Dispose()
    }
    Write-Host 'Tracing configuration restored. Telegram was not stopped.'
    return
}
$script:Executable = (Resolve-Path -LiteralPath $ExePath).ProviderPath
if ([IO.Path]::GetFileName($script:Executable) -cne 'Telegram.exe') { throw 'This collector supports Telegram.exe only.' }
$symbols = (Resolve-Path -LiteralPath $SymbolsDirectory).ProviderPath
$manifest = Assert-BuildPair $script:Executable $symbols
$original = Get-TracingSetting
if (($original.flags -band 1L) -ne 0) { throw 'Heap tracing is already enabled for Telegram.exe. Finish that recording first.' }
if ($CheckOnly) {
    Write-Host "Build pair verified: $($manifest.commit). No tracing or settings changed."
    return
}
if ([Console]::IsInputRedirected) { throw 'Run in an interactive administrator terminal.' }
if (@(Get-ClientProcesses).Count) {
    throw 'Exit every Telegram.exe normally using Quit in its tray menu, then rerun. The script will not terminate clients.'
}
$outputRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$script:CaptureDirectory = Join-Path $outputRoot ('capture-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $script:CaptureDirectory
$script:WprLog = Join-Path $script:CaptureDirectory 'wpr.log'
$statePath = Join-Path $script:CaptureDirectory 'recovery.json'
$script:State = @{
    schema_version = 1
    instance = 'TdMemory' + [guid]::NewGuid().ToString('N')
    original_exists = $original.exists
    original_flags = $original.flags
    tracing_armed = $false
    start_attempted = $false
}
$script:Target = $null
$script:Stages = [Collections.Generic.List[object]]::new()
$script:Deadline = [DateTime]::UtcNow.AddSeconds($MaxSeconds)
Write-JsonFile $statePath $script:State
Write-JsonFile (Join-Path $script:CaptureDirectory 'build-info.json') $manifest
Write-Host "Local output: $script:CaptureDirectory"
Write-Host "If the console is forcibly closed, rerun with -Recover `"$statePath`"."
Write-Host 'VirtualAllocation records system activity. Keep the trace local and filter by the recorded PID in WPA.'
$captureLock = Enter-CaptureLock
try {
    Assert-RecordingBudget
    $current = Get-TracingSetting
    if ($current.exists -ne $original.exists -or $current.flags -ne $original.flags) {
        throw 'TracingFlags changed during preflight. Rerun after the other tracing operation finishes.'
    }
    $script:State.tracing_armed = $true
    Write-JsonFile $statePath $script:State
    Set-TracingSetting $true ($original.flags -bor 1L)
    $script:State.start_attempted = $true
    Write-JsonFile $statePath $script:State
    $null = Invoke-Wpr @('-start', 'Heap', '-start', 'VirtualAllocation', '-filemode', '-recordtempto', $script:CaptureDirectory, '-instancename', $script:State.instance)
    $traceStarted = [DateTime]::UtcNow
    Wait-Stage "Start $script:Executable from your NORMAL desktop/Explorer (not this administrator terminal). Let the chat list settle."
    $processes = @(Get-ClientProcesses)
    if ($processes.Count -ne 1 -or $processes[0].Path -ne $script:Executable) {
        throw 'Expected exactly one Telegram.exe at the specified path.'
    }
    $script:Target = $processes[0]
    $script:TargetStarted = $script:Target.StartTime.ToUniversalTime()
    if ($script:TargetStarted -lt $traceStarted) { throw 'Telegram started before recording was ready. Repeat the capture.' }
    Assert-NormalPrivilege $script:Target.Id
    $null = Assert-BuildPair $script:Executable $symbols
    Save-Stage 'startup_stable'
    Wait-Stage 'Browse the same chats and media that increase memory. Stop playback and close media viewers. Return here before clicking Clean.'
    Save-Stage 'before_cleanup'
    Wait-Stage 'Click Clean once in enhanced settings. Wait until cleanup finishes, then return here without switching chats.'
    $settled = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $settled) {
        Assert-RecordingBudget
        Start-Sleep -Milliseconds 250
    }
    Save-Stage 'after_cleanup'
    $tracePath = Join-Path $script:CaptureDirectory 'memory.etl'
    $null = Invoke-Wpr @('-stop', $tracePath, 'Telegram private commit cleanup', '-skipPdbGen', '-instancename', $script:State.instance)
    $script:State.start_attempted = $false
    Write-JsonFile $statePath $script:State
    Write-Host "Saved trace: $tracePath" -ForegroundColor Green
} finally {
    try {
        Restore-Capture $script:State $statePath
    } finally {
        $captureLock.ReleaseMutex()
        $captureLock.Dispose()
    }
}
