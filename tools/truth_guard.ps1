$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) {
    throw $Message
}

$root = Split-Path -Parent $PSScriptRoot

$readmePath = Join-Path $root 'README.md'
$changelogPath = Join-Path $root 'CHANGELOG.md'
$headerPath = Join-Path $root 'include\nvlog.h'
$matrixPath = Join-Path $root 'docs\audit\STAGE_COMPLETION_MATRIX.md'
$manifestPath = Join-Path $root 'docs\audit\evidence\final_manifest.json'

foreach ($path in @($readmePath, $changelogPath, $headerPath, $matrixPath, $manifestPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        Fail "Missing required file: $path"
    }
}

$readme = Get-Content -LiteralPath $readmePath -Raw
$changelog = Get-Content -LiteralPath $changelogPath -Raw
$header = Get-Content -LiteralPath $headerPath -Raw
$matrix = Get-Content -LiteralPath $matrixPath

if ($readme -match 'active repair') {
    Fail 'README still uses active repair wording after the release pass.'
}

$majorMatch = [regex]::Match($header, '(?m)^#define\s+NVLOG_VERSION_MAJOR\s+(\d+)')
$minorMatch = [regex]::Match($header, '(?m)^#define\s+NVLOG_VERSION_MINOR\s+(\d+)')
$patchMatch = [regex]::Match($header, '(?m)^#define\s+NVLOG_VERSION_PATCH\s+(\d+)')
if (-not ($majorMatch.Success -and $minorMatch.Success -and $patchMatch.Success)) {
    Fail 'Could not parse public version macros from include/nvlog.h.'
}

$version = '{0}.{1}.{2}' -f $majorMatch.Groups[1].Value, $minorMatch.Groups[1].Value, $patchMatch.Groups[1].Value
$escapedVersion = [regex]::Escape($version)
if ($changelog -notmatch "(?m)^##\s+$escapedVersion(?:\s+|$)") {
    Fail "Changelog does not contain a release section for $version."
}
if ($changelog -match '(?m)^##\s+Unreleased(?:\s+|$)') {
    Fail 'Changelog still contains an Unreleased section after the release pass.'
}

$tagNames = @(git tag --list --sort=version:refname)
foreach ($tag in $tagNames) {
    if ($tag -match '^v(\d+\.\d+\.\d+)$') {
        $tagVersion = $Matches[1]
        $escapedTagVersion = [regex]::Escape($tagVersion)
        if ($changelog -notmatch "(?m)^##\s+$escapedTagVersion(?:\s+|$)") {
            Fail "Release tag $tag has no matching changelog release section."
        }
    }
}

$pointedTags = @(git tag --points-at HEAD)
if ($LASTEXITCODE -ne 0) {
    Fail 'git tag --points-at HEAD failed.'
}
if ($pointedTags -notcontains ("v$version")) {
    Fail "HEAD is not tagged with v$version."
}
if ((git cat-file -t "v$version") -ne 'tag') {
    Fail "v$version is not an annotated tag."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.version -ne $version) {
    Fail "Manifest version $($manifest.version) does not match header version $version."
}

$stagePassCount = 0
foreach ($line in $matrix) {
    if ($line -match '^\| (?<stage>\d{2}).* \| PASS \| (?<commit>`[^`]*`|\S+)? \| .* \| (?<evidence>`[^`]*`|\S+)? \|') {
        $commit = $Matches.commit.Trim('`')
        $evidence = $Matches.evidence.Trim('`')
        if ($commit -notmatch '^[0-9a-f]{40}$') {
            Fail "Stage $($Matches.stage) is PASS without a commit SHA."
        }
        if (-not $evidence) {
            Fail "Stage $($Matches.stage) is PASS without an evidence file."
        }
        $evidencePath = Join-Path $root $evidence
        if (-not (Test-Path -LiteralPath $evidencePath)) {
            Fail "Stage $($Matches.stage) evidence file does not exist: $evidence"
        }
        $stagePassCount++
    }
}
if ($stagePassCount -ne 9) {
    Fail "Expected 9 PASS stages in the matrix, found $stagePassCount."
}

$status = git status --short
if ($LASTEXITCODE -ne 0) {
    Fail 'git status --short failed.'
}
if ($status) {
    Fail "Working tree is not clean:`n$status"
}

Write-Host 'truth guard: PASS'
