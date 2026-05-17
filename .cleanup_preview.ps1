Write-Output "== Logical disks =="
wmic logicaldisk get caption,freespace,size | Format-Table -AutoSize
Write-Output ""
Write-Output "== Build\\Debug size and largest files =="
if (Test-Path ".\\build\\Debug") {
  Get-ChildItem -Path ".\\build\\Debug" -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum | ForEach-Object { "BuildDebugSizeGB={0:N2}" -f ($_.Sum/1GB) }
  Get-ChildItem -Path ".\\build\\Debug" -Recurse -File -ErrorAction SilentlyContinue | Sort-Object Length -Descending | Select-Object FullName,@{Name='SizeMB';Expression={[math]::Round($_.Length/1MB,2)}} -First 20 | Format-Table -AutoSize
} else { Write-Output 'No build\\Debug folder' }
Write-Output ""
Write-Output "== Temp files older than 7 days (preview) =="
$old=(Get-Date).AddDays(-7)
$files = Get-ChildItem $env:TEMP -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $old }
if ($files) {
  ($files | Measure-Object Length -Sum | ForEach-Object { "TempOldSizeGB={0:N2}" -f ($_.Sum/1GB) })
  $files | Sort-Object Length -Descending | Select-Object FullName,@{Name='SizeMB';Expression={[math]::Round($_.Length/1MB,2)}},LastWriteTime -First 50 | Format-Table -AutoSize
} else { Write-Output 'No temp files older than 7 days found' }
