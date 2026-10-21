# np185 fetch v3: pure ASCII to avoid PS5.1 GBK parsing issue
$HDC = "D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"
$out = "C:/Users/Administrator/HarmonyOS NEXT Porting Project/d2r-mobile/dm/ohos/app/out"
& $HDC tconn 192.168.3.70:12345 | Out-Null
$dirs = @(
    "/data/storage/el2/base/haps/entry/files",
    "/data/app/el2/100/base/com.example.dmohos/haps/entry/files"
)
foreach ($d in $dirs) {
    Write-Host "--- try $d ---"
    & $HDC shell "ls -la $d" 2>&1 | Select-Object -First 12 | ForEach-Object { Write-Host "  $_" }
}
$f = "dm_gate.txt"
$got = $false
foreach ($d in $dirs) {
    $r = & $HDC file recv "$d/$f" (Join-Path $out $f) 2>&1 | Out-String
    if ($r -notmatch "Fail") { Write-Host "[$f] fetched from $d"; $got = $true; break }
}
if (-not $got) { Write-Host "[$f] NOT FOUND in either path" }
Get-ChildItem $out -Filter "dm_gate.txt" | ForEach-Object { Write-Host "local dm_gate.txt size=$($_.Length) time=$($_.LastWriteTime)" }
