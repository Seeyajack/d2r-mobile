# 一键跑测：构建 → 装机 → 启动 → 点击 → 跑 → 抓日志/轨迹 → 出判据摘要
#
# 为什么要这个脚本（2026-10-19 复盘）：一轮上机 7.4 分钟，其中机器时间只占 1.6 分钟，
# 其余全耗在"手工串六条命令 + 反复 grep"。把六步压成一条，并把"本轮该看什么"自动打出来。
#
# 用法：
#   powershell -File dm/ohos/tools/run_probe.ps1                # 完整一轮
#   powershell -File dm/ohos/tools/run_probe.ps1 -NoBuild       # 只装机重跑（改了 .so 之外的东西时别用）
#   powershell -File dm/ohos/tools/run_probe.ps1 -WaitSec 40    # 跑更久（默认 25 秒）
#
# 输出：<app>/out/last.log（日志）、<app>/out/dm_trace.bin（调用轨迹）、
#       <app>/out/dm_plt_names.txt（符号表），最后打印一段判据摘要。

param(
    [switch]$NoBuild,
    [int]$WaitSec = 25,
    [string]$Device = "192.168.3.70:12345"
)

$ErrorActionPreference = "Stop"
# 控制台按 UTF-8 输出（否则中文在 GBK 控制台上会变乱码）
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8; $OutputEncoding = [Text.Encoding]::UTF8 } catch { }
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))   # …\d2r-mobile
$app  = Join-Path $repo "dm\ohos\app"
$out  = Join-Path $app "out"
$hap  = Join-Path $app "entry\build\default\outputs\default\entry-default-signed.hap"
$devLog = "/data/app/el2/100/base/com.example.dmohos/haps/entry/files/dm_run_test.log"
$devDir = "/data/app/el2/100/base/com.example.dmohos/haps/entry/files"
$bundle = "com.example.dmohos"

$sdk  = "D:\Program Files\Huawei\DevEco Studio\sdk"
$tc   = Join-Path $sdk "default\openharmony\toolchains"
$node = "D:\Program Files\Huawei\DevEco Studio\tools\node\node.exe"
$hvig = "D:\Program Files\Huawei\DevEco Studio\tools\hvigor\bin\hvigorw.js"
$env:DEVECO_SDK_HOME = $sdk
$env:JAVA_HOME = "D:\Program Files\Huawei\DevEco Studio\jbr"
$env:Path = "$($env:JAVA_HOME)\bin;$tc;$($env:Path)"

New-Item -ItemType Directory -Force -Path $out | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()

# ---- 1) 构建（只改 .so 内的 C 源码时也走这条；-NoBuild 可跳过）----
if (-not $NoBuild) {
    Write-Host "[1/6] 构建…" -ForegroundColor Cyan
    Push-Location $app
    # 原生程序往 stderr 写（hvigor 的 WARN 就是这样）会被 ErrorActionPreference=Stop 当成致命错误，
    # 所以这一段临时放宽；成败由输出里有没有 BUILD SUCCESSFUL 判定。
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $buildOut = & $node $hvig --mode module -p product=default -p module=entry@default `
                  -p buildMode=debug assembleHap --no-daemon 2>&1 | Out-String
    $ErrorActionPreference = $prevEap
    Pop-Location
    if ($buildOut -notmatch "BUILD SUCCESSFUL") {
        Write-Host "[失败] 构建没有成功，下面是尾部输出：" -ForegroundColor Red
        ($buildOut | Select-Object -Last 25) | ForEach-Object { Write-Host $_ }
        exit 1
    }
    $b = (Get-Item $hap).LastWriteTime
    Write-Host "       构建成功（HAP $b）" -ForegroundColor Green
}

# ⚠ 2026-10-21 深夜实证：设备同时挂 USB+无线时，裸 hdc 命令报 need connect-key
#   （多目标不限定）。所有 hdc 命令显式 -t $Device。
# ---- 2) 装机（连一次、装一次；失败再连一次重试——真机实测过"传到一半连接掉了"）----
# ⚠ 2026-10-21 实证：**app 还在运行时 `hdc install -r` 会长时间挂住**（今天挂了 6 分半，
#   最后仍失败）。所以装机前先 force-stop；重试也**不要 hdc kill/start**（那会把无线连接也断掉，
#   之后 tconn 直接 Connect failed）。
Write-Host "[2/6] 装机…" -ForegroundColor Cyan
hdc tconn $Device | Out-Null
# ⚠ 2026-10-21 深夜实证：force-stop 会**静默失败**（无线抖动），app 进程熬过整轮
#   ⇒ exp_k 在同一进程里跑第二遍，静态初始化挂在构造器 #0（跨轮状态污染，
#   表现为"游戏比上轮死得还早"）。杀完必须验证，没死干净就重杀 + 报错。
function Stop-App($b) {
    for ($i = 1; $i -le 3; $i++) {
        hdc -t $Device shell "aa force-stop $b" 2>&1 | Out-Null
        Start-Sleep -Seconds 1
        $alive = (hdc -t $Device shell "ps -ef | grep $b | grep -v grep" 2>&1 | Out-String).Trim()
        if (-not $alive) { return $true }
        Write-Host "       第 $i 次 force-stop 后进程仍在，重杀…" -ForegroundColor DarkYellow
    }
    return $false
}
if (-not (Stop-App $bundle)) {
    Write-Host "[失败] force-stop 杀不净 app（进程熬过轮次会污染跨轮状态），中止" -ForegroundColor Red
    exit 1
}
$inst = ""
for ($try = 1; $try -le 3; $try++) {
    $inst = (hdc -t $Device install -r $hap 2>&1 | Out-String)
    if ($inst -match "successfully") { break }
    Write-Host "       第 $try 次装机没成功，重连后重试…" -ForegroundColor DarkYellow
    Start-Sleep -Seconds 2
    hdc tconn $Device 2>&1 | Out-Null
    hdc -t $Device shell "aa force-stop $bundle" 2>&1 | Out-Null
}
if ($inst -notmatch "successfully") {
    Write-Host "[失败] 装机没有成功：" -ForegroundColor Red
    Write-Host $inst
    exit 1
}

# ---- 3) 启动 ----
Write-Host "[3/6] 启动…" -ForegroundColor Cyan
hdc -t $Device shell "aa force-stop $bundle" | Out-Null
hdc -t $Device shell "power-shell wakeup" | Out-Null
hdc -t $Device shell "aa start -a EntryAbility -b $bundle" | Out-Null
Start-Sleep -Seconds 5

# ---- 4) 点击跑测 ----
Write-Host "[4/6] 点击探针按钮…" -ForegroundColor Cyan
hdc -t $Device shell "uinput -T -c 658 1217" | Out-Null

# ---- 5) 等跑完 + 抓产物 ----
Write-Host "[5/6] 等 $WaitSec 秒并抓日志/轨迹…" -ForegroundColor Cyan
Start-Sleep -Seconds $WaitSec
hdc -t $Device file recv $devLog  (Join-Path $out "last.log")       2>&1 | Out-Null
hdc -t $Device file recv "$devDir/dm_trace.bin"      (Join-Path $out "dm_trace.bin")     2>&1 | Out-Null
hdc -t $Device file recv "$devDir/dm_plt_names.txt"  (Join-Path $out "dm_plt_names.txt") 2>&1 | Out-Null

# ---- 6) 判据摘要 ----
$logPath = Join-Path $out "last.log"
if (-not (Test-Path $logPath)) { Write-Host "[失败] 没抓到日志" -ForegroundColor Red; exit 1 }
$log = Get-Content $logPath -Encoding UTF8

Write-Host ""
Write-Host "===== 判据摘要 =====" -ForegroundColor Yellow
# ⚠ 2026-10-21 深夜实证：进程没杀干净时 exp_k 会跑第二遍，静态初始化挂在构造器 #0，
#   整轮数据作废。判据：日志**第一行**的阶段标签必须是「准备」（新鲜进程）；
#   若是「nnMain」⇒ 上一轮的状态漏过来了，本轮结论不可信。
$firstStage = ($log | Select-String -Pattern "^\[阶段 (\w+)\]" | Select-Object -First 1)
if ($firstStage -and $firstStage.Matches.Groups[1].Value -ne "准备") {
    Write-Host "【红】首行阶段=$($firstStage.Matches.Groups[1].Value)（应为「准备」）⇒ 进程不新鲜、本轮数据作废！" -ForegroundColor Red
} else {
    Write-Host "进程新鲜度  ✓ 首行阶段=准备" -ForegroundColor Green
}
# 构建戳（注意：只改 dm_nvn_ohos.c 等其它文件时，这个戳不会更新——它来自 dm_probe.c 的 __TIME__）
($log | Select-String -Pattern "探针构建" | Select-Object -First 1) | ForEach-Object { Write-Host "构建戳  $($_.Line.Trim())" }

$crash = @($log | Select-String -Pattern "nnMain 崩").Count
$ret   = @($log | Select-String -Pattern "nnMain 返回").Count
Write-Host ("结局    崩={0} 返回={1}" -f $crash, $ret) -ForegroundColor $(if ($crash -gt 0) { "Red" } else { "Green" })

if ($crash -gt 0) {
    $log | Select-String -Pattern "崩点现场（nnMain）" | ForEach-Object { Write-Host "崩点    $($_.Line.Trim())" -ForegroundColor Red }
    $log | Select-String -Pattern "\[指令\] pc=" | ForEach-Object { Write-Host "        $($_.Line.Trim())" }
}

# 关键标记：可按本轮关心的事改这一行（np143：单例 A/B 创建时机观测）
$marks = "mgr-契约|工厂 0x1f8|管理器\] 虚方法|启动包门|AB观测|0x1f8 崩|nnMain|Volante\] 帧循环|TRACE\]"
($log | Select-String -Pattern $marks | Select-Object -First 12) | ForEach-Object {
    Write-Host "  · $($_.Line.Trim())"
}

# 导入调用统计（来自 dm_plt_names + trace 的分析结果，若有）
$tr = Join-Path $out "dm_trace.bin"
if (Test-Path $tr) {
    Write-Host ("轨迹    {0:N0} KB" -f ((Get-Item $tr).Length / 1KB))
} else {
    Write-Host "轨迹    没抓到（游戏可能还没跑到落盘点）" -ForegroundColor DarkYellow
}

$sw.Stop()
Write-Host ""
Write-Host ("总耗时 {0:N0} 秒；产物在 {1}" -f $sw.Elapsed.TotalSeconds, $out) -ForegroundColor Cyan
