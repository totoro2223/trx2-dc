# Brings a newer TRX into this checkout's branch, then builds and plays every
# level of TR1, TR2 and TR3 (test_levels.ps1) to show nothing broke.
#
#   .\tools\dreamcast\update_base.ps1                    # TRX's develop
#   .\tools\dreamcast\update_base.ps1 -From upstream/stable
#   .\tools\dreamcast\update_base.ps1 -From dreamcast    # in the refactor
#                                                        # worktree, after
#                                                        # updating dreamcast
#
# Steps: refuses a dirty tree; fetches TRX (remote "upstream", added if
# missing); tags the branch as it was (pre-update-<date>); merges -From.
# - A conflict stops it with the merge left open: the conflicted files are
#   listed in build/update-conflicts.txt. Resolve them (keep the Dreamcast
#   side in #if defined(__DREAMCAST__) blocks and src/platform/dreamcast),
#   `git commit`, and run it again with -NoMerge to build and test; or undo
#   with `git merge --abort`.
# - A failed build or level pass leaves the merge committed: undo it with
#   `git reset --hard pre-update-<date>`.
param(
    [string]$From = 'upstream/develop',
    [string]$UpstreamUrl = 'https://github.com/LostArtefacts/TRX.git',
    [string[]]$Games = @('tr1', 'tr2', 'tr3'),
    [switch]$NoMerge,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
Set-Location $root
if (-not $env:DREAMSDK_HOME) { $env:DREAMSDK_HOME = 'D:\DreamSDK' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$branch = git branch --show-current

if (-not $NoMerge) {
    if (git status --porcelain --untracked-files=no) {
        'The tree has uncommitted changes: commit or set them aside first.'
        exit 1
    }
    if ($From -like 'upstream/*') {
        if (-not (git remote | Select-String -SimpleMatch -Pattern 'upstream' -Quiet)) {
            git remote add upstream $UpstreamUrl
        }
        "== fetching $UpstreamUrl"
        git fetch upstream --no-tags
        if ($LASTEXITCODE -ne 0) { 'fetch failed'; exit 1 }
    }
    $tag = "pre-update-$stamp"
    git tag $tag
    "== $branch tagged $tag; merging $From ($(git log -1 --format='%h %s' $From))"
    git merge --no-edit $From
    if ($LASTEXITCODE -ne 0) {
        $conflicts = git diff --name-only --diff-filter=U
        @(
            "Merge of $From into $branch ($stamp) stopped on conflicts."
            "Undo: git merge --abort   Before: $tag"
            ''
            $conflicts
        ) | Set-Content -Encoding utf8 build/update-conflicts.txt
        "CONFLICTS in $(@($conflicts).Count) files (build/update-conflicts.txt):"
        $conflicts
        exit 2
    }
}

'== building (minsize), log in build/update-build.log'
& "$PSScriptRoot\dcsh.ps1" "bash tools/dreamcast/build.sh minsize > build/update-build.log 2>&1"
if ($LASTEXITCODE -ne 0) {
    'BUILD FAILED:'
    Select-String -Path build/update-build.log -Pattern 'error:|FAILED' | Select-Object -First 20 | ForEach-Object { $_.Line }
    exit 3
}

if ($SkipTests) { 'Built; tests skipped.'; exit 0 }
& "$PSScriptRoot\test_levels.ps1" -Games $Games -Tag "update-$stamp"
exit $LASTEXITCODE
