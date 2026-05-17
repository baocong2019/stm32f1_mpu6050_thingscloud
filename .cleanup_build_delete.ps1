$target = Resolve-Path -Path .\build\Debug -ErrorAction SilentlyContinue
if (-not $target) { Write-Output 'No build\Debug folder found'; exit }
$target = $target.Path
Write-Output "Deleting all items in: $target"
$entries = Get-ChildItem -Path $target -Force -ErrorAction SilentlyContinue
$deletedCount = 0
$sum = 0
$failed = @()
foreach ($e in $entries) {
  try {
    if ($e.PSIsContainer) {
      $size = (Get-ChildItem -Path $e.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    } else {
      $size = $e.Length
    }
    if (-not $size) { $size = 0 }
    Remove-Item -LiteralPath $e.FullName -Recurse -Force -ErrorAction Stop
    $deletedCount++
    $sum += $size
  } catch {
    $failed += $e.FullName
  }
}
Write-Output "DeletedCount=$deletedCount"
Write-Output ("DeletedBytes={0}" -f $sum)
Write-Output ("DeletedGB={0:N2}" -f ($sum/1GB))
if ($failed.Count -gt 0) {
  Write-Output "FailedCount=$($failed.Count)"
  $failed | ForEach-Object { Write-Output "FAILED: $_" }
} else {
  Write-Output "No failures"
}
