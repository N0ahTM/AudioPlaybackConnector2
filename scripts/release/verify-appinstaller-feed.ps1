param(
    [Parameter(Mandatory = $true)]
    [string]$AppInstallerUrl,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedPackageVersion,

    [string]$ExpectedPackageName = "",
    [string]$ExpectedPublisher = "",
    [string]$ExpectedProcessorArchitecture = "",
    [string]$ExpectedPackageUri = "",
    [switch]$VerifyPackageUri,

    [int]$Attempts = 6,
    [int]$DelaySeconds = 10,
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$verified = $false
$packageUri = ""

for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
    try {
        $response = Invoke-WebRequest -Uri $AppInstallerUrl -UseBasicParsing -TimeoutSec $TimeoutSeconds -ErrorAction Stop
        $content = if ($response.Content -is [byte[]]) {
            [System.Text.Encoding]::UTF8.GetString($response.Content)
        } else {
            [string]$response.Content
        }

        [xml]$feed = $content
        $appInstallerVersion = $feed.DocumentElement.GetAttribute("Version")
        $packageNode = $feed.DocumentElement.GetElementsByTagName("MainPackage", $feed.DocumentElement.NamespaceURI)[0]
        if (-not $packageNode) {
            throw "MainPackage element not found."
        }

        $actualPackageVersion = $packageNode.GetAttribute("Version")
        $actualPackageName = $packageNode.GetAttribute("Name")
        $actualPublisher = $packageNode.GetAttribute("Publisher")
        $actualArchitecture = $packageNode.GetAttribute("ProcessorArchitecture")
        $packageUri = $packageNode.GetAttribute("Uri")
        $matches = $appInstallerVersion -eq $ExpectedPackageVersion -and
            $actualPackageVersion -eq $ExpectedPackageVersion -and
            ([string]::IsNullOrWhiteSpace($ExpectedPackageName) -or $actualPackageName -eq $ExpectedPackageName) -and
            ([string]::IsNullOrWhiteSpace($ExpectedPublisher) -or $actualPublisher -eq $ExpectedPublisher) -and
            ([string]::IsNullOrWhiteSpace($ExpectedProcessorArchitecture) -or $actualArchitecture -eq $ExpectedProcessorArchitecture) -and
            ([string]::IsNullOrWhiteSpace($ExpectedPackageUri) -or $packageUri -eq $ExpectedPackageUri)
        if ($matches) {
            $verified = $true
            break
        }

        Write-Warning "App Installer feed metadata did not match the expected package identity and URI."
    } catch {
        Write-Warning "App Installer feed check failed on attempt ${attempt}: $($_.Exception.Message)"
    }

    if ($attempt -lt $Attempts) {
        Start-Sleep -Seconds $DelaySeconds
    }
}

if (-not $verified) {
    throw "Published App Installer feed did not match package version $ExpectedPackageVersion."
}

Write-Host "App Installer feed verified: $AppInstallerUrl -> $ExpectedPackageVersion"

if ($VerifyPackageUri) {
    $packageReachable = $false
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            $response = Invoke-WebRequest -Uri $packageUri -Method Head -UseBasicParsing -TimeoutSec $TimeoutSeconds -ErrorAction Stop
            if ([int]$response.StatusCode -ge 200 -and [int]$response.StatusCode -lt 400) {
                $packageReachable = $true
                break
            }
        } catch {
            Write-Warning "Published MSIX check failed on attempt ${attempt}: $($_.Exception.Message)"
        }
        if ($attempt -lt $Attempts) {
            Start-Sleep -Seconds $DelaySeconds
        }
    }
    if (-not $packageReachable) {
        throw "Published MSIX is not publicly reachable at $packageUri."
    }
    Write-Host "Published MSIX is publicly reachable: $packageUri"
}
