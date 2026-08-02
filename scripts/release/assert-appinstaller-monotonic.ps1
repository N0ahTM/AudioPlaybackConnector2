param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateVersion,

    [string]$AppInstallerUrl = "",
    [string]$ExistingFeedPath = "",
    [int]$Attempts = 3,
    [int]$DelaySeconds = 5,
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"

function Convert-PackageVersion {
    param([string]$Value)

    $parts = $Value.Split(".")
    if ($parts.Count -ne 4) {
        throw "Package version '$Value' must have four components."
    }

    $result = @()
    foreach ($part in $parts) {
        [uint64]$number = 0
        if (-not [uint64]::TryParse($part, [ref]$number) -or $number -gt [uint16]::MaxValue) {
            throw "Package version '$Value' contains an invalid component."
        }
        $result += $number
    }
    return $result
}

function Compare-PackageVersion {
    param([uint64[]]$Left, [uint64[]]$Right)

    for ($index = 0; $index -lt 4; $index++) {
        if ($Left[$index] -lt $Right[$index]) { return -1 }
        if ($Left[$index] -gt $Right[$index]) { return 1 }
    }
    return 0
}

$candidate = Convert-PackageVersion -Value $CandidateVersion
$feedContent = $null

if (-not [string]::IsNullOrWhiteSpace($ExistingFeedPath)) {
    $feedContent = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $ExistingFeedPath).Path)
} else {
    if ([string]::IsNullOrWhiteSpace($AppInstallerUrl)) {
        throw "AppInstallerUrl is required when ExistingFeedPath is not supplied."
    }

    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            $response = Invoke-WebRequest -Uri $AppInstallerUrl -UseBasicParsing -TimeoutSec $TimeoutSeconds -ErrorAction Stop
            $feedContent = if ($response.Content -is [byte[]]) {
                [System.Text.Encoding]::UTF8.GetString($response.Content)
            } else {
                [string]$response.Content
            }
            break
        } catch {
            $statusCode = if ($null -ne $_.Exception.Response -and
                $null -ne $_.Exception.Response.StatusCode) {
                [int]$_.Exception.Response.StatusCode
            } else {
                0
            }
            if ($statusCode -eq 404) {
                Write-Host "No existing App Installer feed was found; candidate $CandidateVersion is the first version."
                return
            }
            if ($attempt -eq $Attempts) { throw }
            Write-Warning "Existing feed check failed on attempt ${attempt}: $($_.Exception.Message)"
            Start-Sleep -Seconds $DelaySeconds
        }
    }
}

[xml]$feed = $feedContent
$rootVersion = $feed.DocumentElement.GetAttribute("Version")
$packageNode = $feed.DocumentElement.GetElementsByTagName("MainPackage", $feed.DocumentElement.NamespaceURI)[0]
if (-not $packageNode) {
    throw "Existing App Installer feed has no MainPackage element."
}
$packageVersion = $packageNode.GetAttribute("Version")
if ($rootVersion -ne $packageVersion) {
    throw "Existing App Installer feed is inconsistent: root version '$rootVersion', package version '$packageVersion'."
}

$current = Convert-PackageVersion -Value $packageVersion
if ((Compare-PackageVersion -Left $candidate -Right $current) -lt 0) {
    throw "Refusing to replace App Installer version $packageVersion with older version $CandidateVersion."
}

Write-Host "App Installer version is monotonic: $packageVersion -> $CandidateVersion"
