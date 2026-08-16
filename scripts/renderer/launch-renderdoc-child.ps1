param(
    [Parameter(Mandatory = $true)]
    [string] $Viewer,

    [Parameter(Mandatory = $true)]
    [string] $ArgumentsFile
)

$ErrorActionPreference = 'Stop'

$viewerPath = [IO.Path]::GetFullPath($Viewer)
$argumentPath = [IO.Path]::GetFullPath($ArgumentsFile)
$viewerArguments = @(Get-Content -LiteralPath $argumentPath -Raw | ConvertFrom-Json)

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $viewerPath
$startInfo.WorkingDirectory = Split-Path -Parent $viewerPath
$startInfo.UseShellExecute = $false

foreach ($argument in $viewerArguments) {
    [void]$startInfo.ArgumentList.Add([string]$argument)
}

$viewerProcess = [Diagnostics.Process]::Start($startInfo)
$viewerProcess.WaitForExit()
exit $viewerProcess.ExitCode
