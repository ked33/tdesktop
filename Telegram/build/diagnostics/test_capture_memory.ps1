#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest

$source = Join-Path $PSScriptRoot 'capture_memory.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors -join [Environment]::NewLine) }
foreach ($definition in $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($definition.Extent.Text))
}

function Assert-Equal($Actual, $Expected) {
    if ($Actual -cne $Expected) { throw "Expected '$Expected', got '$Actual'." }
}

function Assert-Throws([scriptblock] $Action, [string] $Message) {
    try { & $Action } catch {
        if ($_.Exception.Message -notlike "*$Message*") { throw }
        return
    }
    throw "Expected failure containing: $Message"
}

function New-State {
    return @{
        schema_version = 1
        instance = 'TdMemory' + ('a' * 32)
        original_exists = $false
        original_flags = [long]0
        tracing_armed = $true
        start_attempted = $true
    }
}

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryRoot ('td-memory-test-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $fixture
try {
    function Get-ClientProcesses { return $script:FakeClients }
    $script:FakeClients = @()
    Assert-Equal @(Assert-ClientsStopped '文件校验前').Count 0
    $script:FakeClients = @([pscustomobject]@{
        Id = 123
        Path = 'D:\Client One\Telegram.exe'
        StartTime = [datetime]'2026-11-10T21:26:03'
    })
    Assert-Throws { Assert-ClientsStopped '文件校验前' } '文件校验前检测到'
    Assert-Throws { Assert-ClientsStopped '文件校验前' } 'PID=123 started=2026-11-10 21:26:03 path=D:\Client One\Telegram.exe'
    $script:FakeClients += [pscustomobject]@{
        Id = 456
        Path = 'D:\Client Two\Telegram.exe'
        StartTime = [datetime]'2026-11-10T21:27:00'
    }
    Assert-Throws { Assert-ClientsStopped '文件校验后' } 'PID=123*PID=456'
    $inaccessible = [pscustomobject]@{ Id = 789 }
    $inaccessible | Add-Member ScriptProperty Path { throw 'Access denied' }
    $inaccessible | Add-Member ScriptProperty StartTime { throw 'Process exited' }
    $script:FakeClients = @($inaccessible)
    Assert-Throws { Assert-ClientsStopped '启用跟踪前' } 'PID=789*<无法读取启动时间>*<无法读取路径>'
    $script:FakeClients = @()
    Assert-Equal @(Assert-ClientsStopped '文件校验后').Count 0

    function Read-CaptureKey { return $script:FakeKeys.Dequeue() }
    function Assert-RecordingBudget { $script:BudgetChecks++ }
    $script:FakeKeys = [Collections.Generic.Queue[object]]::new()
    foreach ($key in @([ConsoleKey]::Enter, $null, [ConsoleKey]::Enter)) { $script:FakeKeys.Enqueue($key) }
    $script:BudgetChecks = 0
    Wait-Stage 'Fixture stage: discard Enter buffered before this prompt.'
    Assert-Equal $script:FakeKeys.Count 0
    Assert-Equal $script:BudgetChecks 1
    $script:FakeKeys.Enqueue([ConsoleKey]::Q)
    Assert-Throws { Wait-Stage 'Fixture buffered cancellation.' } 'cancelled by the operator'
    Assert-Equal $script:BudgetChecks 1
    $script:FakeKeys.Enqueue($null)
    $script:FakeKeys.Enqueue([ConsoleKey]::Q)
    Assert-Throws { Wait-Stage 'Fixture cancellation after prompt.' } 'cancelled by the operator'
    Assert-Equal $script:BudgetChecks 2

    $state = New-State
    Assert-CaptureState $state
    Assert-Equal (Get-RestoreAction $state @{ exists = $false; flags = [long]0 }) 'None'
    Assert-Equal (Get-RestoreAction $state @{ exists = $true; flags = [long]1 }) 'Restore'
    Assert-Throws { Get-RestoreAction $state @{ exists = $true; flags = [long]5 } } 'changed outside'
    $state.original_exists = $true
    $state.original_flags = [long]2147483650
    Assert-Equal (Get-RestoreAction $state @{ exists = $true; flags = [long]2147483651 }) 'Restore'
    Assert-Equal (Get-RestoreAction $state @{ exists = $true; flags = [long]2147483650 }) 'None'
    $state.instance = 'OtherRecording'
    Assert-Throws { Assert-CaptureState $state } 'instance'
    $state = New-State
    $state.original_flags = -1L
    Assert-Throws { Assert-CaptureState $state } 'DWORD range'
    $state.original_flags = 4294967296L
    Assert-Throws { Assert-CaptureState $state } 'DWORD range'
    $state = New-State
    $state.tracing_armed = 'true'
    Assert-Throws { Assert-CaptureState $state } 'tracing_armed'

    $exe = Join-Path $fixture 'Telegram.exe'
    $pdb = Join-Path $fixture 'Telegram.pdb'
    [IO.File]::WriteAllText($exe, 'fixture executable')
    [IO.File]::WriteAllText($pdb, 'fixture symbols')
    $manifest = @{
        schema_version = 1
        architecture = 'x64'
        commit = 'test'
        files = @{}
    }
    foreach ($file in @($exe, $pdb)) {
        $manifest.files[[IO.Path]::GetFileName($file)] = @{
            sha256 = (Get-FileHash -LiteralPath $file).Hash
            size = (Get-Item -LiteralPath $file).Length
        }
    }
    $manifestPath = Join-Path $fixture 'build-info.json'
    Write-JsonFile $manifestPath $manifest
    Assert-Equal (Assert-BuildPair $exe $fixture).commit 'test'
    [IO.File]::WriteAllText($pdb, 'different symbols')
    Assert-Throws { Assert-BuildPair $exe $fixture } 'Telegram.pdb does not match'
    [IO.File]::WriteAllText($pdb, 'fixture symbols')
    [IO.File]::WriteAllText($exe, 'different executable')
    Assert-Throws { Assert-BuildPair $exe $fixture } 'Telegram.exe does not match'
    [IO.File]::WriteAllText($exe, 'fixture executable')
    $manifest.architecture = 'arm64'
    Write-JsonFile $manifestPath $manifest
    Assert-Throws { Assert-BuildPair $exe $fixture } 'x64'

    $script:WprPath = (Get-Command pwsh).Source
    $script:WprLog = Join-Path $fixture 'wpr.log'
    Assert-Equal (Invoke-Wpr @('-NoProfile', '-Command', 'exit 7') -AllowFailure).exit_code 7
    Assert-Throws { Invoke-Wpr @('-NoProfile', '-Command', 'exit 9') } 'WPR failed (9)'
    Assert-Equal (Invoke-Wpr @('-NoProfile', '-Command', 'exit 0')).exit_code 0

    $stagesPath = Join-Path $fixture 'stages.json'
    Write-JsonFile $stagesPath @(@{ stage = 'startup_stable' })
    Assert-Equal (Get-Content -LiteralPath $stagesPath -Raw).Trim().StartsWith('[') $true

    function Get-TracingSetting { return $script:FakeTracing }
    function Set-TracingSetting($Exists, [long] $Flags) {
        $script:FakeTracing = @{ exists = $Exists; flags = $Flags }
        $script:Writes++
    }
    function Invoke-Wpr([string[]] $Arguments, [switch] $AllowFailure) {
        Assert-Equal ($Arguments -join ' ') ('-cancel -instancename TdMemory' + ('a' * 32))
        $script:Cancels++
        return @{ exit_code = $script:CancelExit; output = $script:CancelOutput }
    }
    $script:FakeTracing = @{ exists = $true; flags = [long]1 }
    $script:Writes = 0
    $script:Cancels = 0
    $script:CancelExit = 0
    $script:CancelOutput = ''
    $state = New-State
    $journal = Join-Path $fixture 'recovery.json'
    Restore-Capture $state $journal
    Assert-Equal $script:Writes 1
    Assert-Equal $script:Cancels 1
    Assert-Equal $script:FakeTracing.exists $false
    Restore-Capture $state $journal
    Assert-Equal $script:Writes 1
    Assert-Equal $script:Cancels 1
    $saved = Get-Content -LiteralPath $journal -Raw | ConvertFrom-Json -AsHashtable
    Assert-Equal $saved.tracing_armed $false
    Assert-Equal $saved.start_attempted $false

    $script:FakeTracing = @{ exists = $true; flags = [long]5 }
    $state = New-State
    Assert-Throws { Restore-Capture $state $journal } 'changed outside'
    Assert-Equal $script:FakeTracing.flags 5
    Assert-Equal $state.tracing_armed $true
    Assert-Equal $state.start_attempted $false
    Assert-Equal $script:Cancels 2

    $script:FakeTracing = @{ exists = $true; flags = [long]1 }
    $script:CancelExit = 1
    $state = New-State
    Assert-Throws { Restore-Capture $state $journal } 'Could not confirm cancellation'
    Assert-Equal $state.tracing_armed $false
    Assert-Equal $state.start_attempted $true
    Assert-Equal $script:FakeTracing.exists $false
    $script:CancelOutput = 'There are no trace profiles running. Error code: 0xc5583000'
    Restore-Capture $state $journal
    Assert-Equal $state.start_attempted $false
    $script:CancelExit = 0
    $script:FakeTracing = @{ exists = $true; flags = [long]2147483651 }
    $state = New-State
    $state.original_exists = $true
    $state.original_flags = [long]2147483650
    Restore-Capture $state $journal
    Assert-Equal $script:FakeTracing.exists $true
    Assert-Equal $script:FakeTracing.flags 2147483650
    Write-Host 'Capture checks passed: process preflight, stage input, build pairing, command failures, recovery ownership, conflicts and retry state.'
} finally {
    $resolved = (Resolve-Path -LiteralPath $fixture).ProviderPath
    if ($resolved -ne $fixture -or -not $resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a fixture outside its temporary root.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
