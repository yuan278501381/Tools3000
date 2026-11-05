# Shared release-directory cleanup policy used by both release entry points.
function Remove-ProjectArtifactDirectorySafely {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $RootPath = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\')
    $TargetPath = [System.IO.Path]::GetFullPath((Join-Path $RootPath $RelativePath))
    $AllowedPaths = @("Output", "deploy_dist", "ui\dist", "release_assets") |
        ForEach-Object { [System.IO.Path]::GetFullPath((Join-Path $RootPath $_)) }
    if (-not ($AllowedPaths | Where-Object {
            $_.Equals($TargetPath, [System.StringComparison]::OrdinalIgnoreCase)
        })) {
        throw "Refusing to clean a directory outside the release artifact allowlist: $TargetPath"
    }
    # Check every component, including ui/ for ui/dist, before recursive removal.
    $CurrentPath = $TargetPath
    while (-not $CurrentPath.Equals($RootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $CurrentPath) {
            $Item = Get-Item -LiteralPath $CurrentPath -Force
            if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to clean through a reparse point: $CurrentPath"
            }
        }
        $CurrentPath = [System.IO.Path]::GetDirectoryName($CurrentPath)
    }
    if (Test-Path -LiteralPath $TargetPath) {
        Remove-Item -LiteralPath $TargetPath -Recurse -Force
    }
}
