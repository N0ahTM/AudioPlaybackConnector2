param(
    [Parameter(Mandatory = $true)]
    [string]$AppInstallerUrl,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedVersion,

    [int]$Attempts = 6,
    [int]$DelaySeconds = 10,
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$verified = $false

for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
    try {
        $response = Invoke-WebRequest -Uri $AppInstallerUrl -UseBasicParsing -TimeoutSec $TimeoutSeconds -ErrorAction Stop
        $content = if ($response.Content -is [byte[]]) {
            [System.Text.Encoding]::UTF8.GetString($response.Content)
        } else {
            [string]$response.Content
        }

        [xml]$feed = $content
        $actualVersion = $feed.DocumentElement.GetAttribute("Version")
        if ($actualVersion -eq $ExpectedVersion) {
            $verified = $true
            break
        }

        Write-Warning "App Installer feed version is '$actualVersion', expected '$ExpectedVersion'."
    } catch {
        Write-Warning "App Installer feed check failed on attempt ${attempt}: $($_.Exception.Message)"
    }

    if ($attempt -lt $Attempts) {
        Start-Sleep -Seconds $DelaySeconds
    }
}

if (-not $verified) {
    throw "Published App Installer feed did not match version $ExpectedVersion."
}

Write-Host "App Installer feed verified: $AppInstallerUrl -> $ExpectedVersion"
