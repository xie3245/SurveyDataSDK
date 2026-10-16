param(
    [string]$BuildDirectory = "build",
    [string]$Configuration = "Release",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot $BuildDirectory
}
$candidates = @(
    (Join-Path (Join-Path $BuildDirectory $Configuration) "survey_exchange.exe"),
    (Join-Path $BuildDirectory "survey_exchange.exe")
)
$tool = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $tool) {
    throw "Build the project first. See START_HERE.txt for the CMake commands."
}
if ([string]::IsNullOrEmpty($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot ("roundtrip-" + [guid]::NewGuid().ToString("N").Substring(0, 12))
}
if ((Test-Path -LiteralPath $OutputDirectory) -and (Get-ChildItem -LiteralPath $OutputDirectory -Force | Select-Object -First 1)) {
    throw "Choose a new or empty output directory; existing files will be kept."
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$appA = Join-Path $OutputDirectory "app-a.db"
$appB = Join-Path $OutputDirectory "app-b.db"
$secondB = Join-Path $OutputDirectory "app-b-second-checkout.db"
$checkout = Join-Path $OutputDirectory "checkout.json"
$returned = Join-Path $OutputDirectory "returned.json"
$secondCheckout = Join-Path $OutputDirectory "second-checkout.json"
$staleReturn = Join-Path $OutputDirectory "stale-return.json"

function Invoke-Tool {
    param([string[]]$ToolArguments, [int]$ExpectedExit = 0)
    & $tool @ToolArguments
    if ($LASTEXITCODE -ne $ExpectedExit) {
        throw "Command returned $LASTEXITCODE; expected $ExpectedExit."
    }
}

Write-Host "Successful exchange: App A -> App B -> App A"
Invoke-Tool -ToolArguments @("seed", $appA)
Invoke-Tool -ToolArguments @("export", $appA, "app-a", "demo-batch", $checkout)
Invoke-Tool -ToolArguments @("checkout", $appB, $checkout)
Invoke-Tool -ToolArguments @("append", $appB, "demo-batch", "from-b", "10", "20", "30")
Invoke-Tool -ToolArguments @("return", $appB, $checkout, $returned)
Invoke-Tool -ToolArguments @("apply", $appA, "app-a", $returned)
Invoke-Tool -ToolArguments @("show", $appA, "demo-batch")

Write-Host "Conflict example: App A changes while App B is working on an older copy"
Invoke-Tool -ToolArguments @("export", $appA, "app-a", "demo-batch", $secondCheckout)
Invoke-Tool -ToolArguments @("checkout", $secondB, $secondCheckout)
Invoke-Tool -ToolArguments @("append", $secondB, "demo-batch", "offline-point", "40", "50", "60")
Invoke-Tool -ToolArguments @("return", $secondB, $secondCheckout, $staleReturn)
Invoke-Tool -ToolArguments @("append", $appA, "demo-batch", "newer-in-a", "70", "80", "90")
Invoke-Tool -ToolArguments @("apply", $appA, "app-a", $staleReturn) -ExpectedExit 2
Invoke-Tool -ToolArguments @("show", $appA, "demo-batch")
Write-Host "The CONFLICT above is expected. Newer data in App A was kept."
Write-Host "Inspect the JSON files and separate app databases in: $OutputDirectory"
