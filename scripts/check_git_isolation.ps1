# Verify no libgit2 includes leak outside libs/vxcore/src/sync/git/
#
# Exit 0: PASS (boundary maintained)
# Exit 1: FAIL (forbidden includes found; list printed)
#
# Run from repo root:
#   & "libs/vxcore/scripts/check_git_isolation.ps1"

$ErrorActionPreference = 'Stop'

# Collect all .h/.cpp files under libs/vxcore/src EXCEPT those inside sync/git/.
$candidates = Get-ChildItem -Path "libs/vxcore/src" -Recurse -Include *.h,*.cpp -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '[\\/]sync[\\/]git[\\/]' }

if (-not $candidates) {
    Write-Output "PASS: no source files found under libs/vxcore/src (nothing to check)"
    exit 0
}

# Look for any #include of <git2.h> / "git2.h" / libgit2 headers in those files.
$matches = $candidates | Select-String -Pattern '#include\s+[<"]\s*(git2|libgit2)' -ErrorAction SilentlyContinue

if (-not $matches) {
    Write-Output "PASS: libgit2 isolation maintained (no git2/libgit2 includes outside libs/vxcore/src/sync/git/)"
    exit 0
}

Write-Output "FAIL: libgit2 includes found outside libs/vxcore/src/sync/git/:"
$matches | ForEach-Object {
    Write-Output ("  {0}:{1}: {2}" -f $_.Path, $_.LineNumber, $_.Line.Trim())
}
exit 1
