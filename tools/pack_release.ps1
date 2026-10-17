<#
  愚见者的故事书 · 一键打包（版本号 v大.中.小）
  ------------------------------------------------------------------
  把当前游戏（愚见者的故事书.exe + stories 里的剧本 + images 里的图片）
  封成一个单独的 exe，发给别人双击就能玩。

  版本号规则（记在项目根目录的「版本日志.txt」里，每打包一次加一行）：

      v 大.中.小
        │  │  └─ 小版本：其它改动都算小版本（**包括改代码 / 引擎逻辑**、剧情文字、图片）
        │  └──── 中版本：剧本有增减（加了 / 删了故事）
        └─────── 大版本：**由你自己说了算**
                  在 版本日志.txt 里有一行「大版本 0」，你把它改成几，下一个版本就是
                  v几.0.0（大版本变更会重置中/小号）。测试阶段就一直留 0。

  脚本会自动记录代码指纹（main.cpp + launcher.cpp）和内容指纹，
  只是为了在日志里注明"这版动了什么"，不影响版本号怎么涨。

  产物名字带版本号：发布\愚见者的故事书-v0.0.3.exe

  用法：
      powershell -ExecutionPolicy Bypass -File tools\pack_release.ps1
      powershell -ExecutionPolicy Bypass -File tools\pack_release.ps1 -Build
      powershell -ExecutionPolicy Bypass -File tools\pack_release.ps1 -OutDir D:\
      powershell -ExecutionPolicy Bypass -File tools\pack_release.ps1 -Name 送人版.exe
      powershell -ExecutionPolicy Bypass -File tools\pack_release.ps1 -NoImages

  也可以直接双击 tools\生成安装包.cmd。
#>

param(
    [string]$Name = "",
    [string]$OutDir = "",
    [switch]$Build,
    [switch]$NoImages
)

$ErrorActionPreference = "Stop"

function Info($msg) { Write-Host $msg -ForegroundColor Cyan }
function Warn($msg) { Write-Host $msg -ForegroundColor Yellow }
function Bad($msg) { Write-Host $msg -ForegroundColor Red }

function Get-StringHash([string]$text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').Substring(0, 10)
    }
    finally { $sha.Dispose() }
}

$root = Split-Path -Parent $PSScriptRoot
$gameExe = Join-Path $root "愚见者的故事书.exe"
$storiesDir = Join-Path $root "stories"
$imagesDir = Join-Path $root "images"
$logPath = Join-Path $root "版本日志.txt"
$versionFile = Join-Path $root "版本.txt"
$mainSrc = Join-Path $root "main.cpp"
$stubPath = Join-Path $PSScriptRoot "launcher_stub.exe"
$stubSrc = Join-Path $PSScriptRoot "launcher.cpp"

if ($OutDir -eq "") { $OutDir = Join-Path $root "发布" }

# ---------- 1. 需要的话先重新编译游戏 ----------
if ($Build) {
    $gpp = Get-Command g++ -ErrorAction SilentlyContinue
    if ($gpp) {
        Info "重新编译游戏（-static，保证别人机器上不缺 DLL）..."
        $buildArgs = @("-std=c++17", "-mwindows", "-O2", "-static", "-s", "-o", $gameExe, $mainSrc,
                       "-lgdi32", "-luser32", "-lgdiplus", "-lole32", "-lshell32", "-lmsimg32")
        & $gpp.Source @buildArgs
        if ($LASTEXITCODE -ne 0) { Bad "编译失败，改用现有的 exe。" }
        else { Info "编译完成。" }
    }
    else {
        Warn "没找到 g++，跳过编译，直接用现有的 exe。"
    }
}
if (-not (Test-Path -LiteralPath $gameExe)) {
    Bad "找不到 $gameExe，请先编译游戏（VS Code 里按 Ctrl+Shift+B）。"
    exit 1
}

# ---------- 2. 准备启动器 ----------
# 每次都重编一份新的 launcher_stub_built.exe：这样改过 launcher.cpp 一定生效，
# 也不会因为旧的 launcher_stub.exe 被占用 / 改不动而一直用老的。
$stubFresh = Join-Path (Split-Path $stubPath -Parent) "launcher_stub_built.exe"
$gpp = Get-Command g++ -ErrorAction SilentlyContinue
if ($gpp) {
    Info "编译启动器 launcher_stub_built.exe ..."
    & $gpp.Source "-std=c++17" "-mwindows" "-O2" "-s" "-static" "-o" $stubFresh $stubSrc
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $stubFresh)) {
        $stubPath = $stubFresh
    } else {
        Warn "启动器编译失败，继续用现有的 launcher_stub.exe。"
    }
}
if (-not (Test-Path -LiteralPath $stubPath)) {
    Bad "缺少启动器，而且没能编译出来（需要 g++）。"
    exit 1
}

# ---------- 3. 收集要打包的文件 ----------
$files = New-Object System.Collections.Generic.List[object]
function Add-File([string]$rel, [string]$full) {
    $files.Add([pscustomobject]@{ Rel = $rel; Full = $full })
}

Add-File "愚见者的故事书.exe" $gameExe
$readme = Join-Path $root "README.md"
if (Test-Path -LiteralPath $readme) { Add-File "README.md" $readme }

if (Test-Path -LiteralPath $storiesDir) {
    Get-ChildItem -LiteralPath $storiesDir -File |
        Where-Object { $_.Extension -eq ".txt" } |
        ForEach-Object { Add-File ("stories\" + $_.Name) $_.FullName }
}

if (-not $NoImages -and (Test-Path -LiteralPath $imagesDir)) {
    $imageExt = @(".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff", ".webp")
    # images\ 根目录下的图也要带上（比如开场用的 title.png）
    Get-ChildItem -LiteralPath $imagesDir -File |
        Where-Object { $imageExt -contains $_.Extension.ToLower() } |
        ForEach-Object { Add-File ("images\" + $_.Name) $_.FullName }
    Get-ChildItem -LiteralPath $imagesDir -Directory | ForEach-Object {
        $story = $_
        Get-ChildItem -LiteralPath $story.FullName -File |
            Where-Object { $imageExt -contains $_.Extension.ToLower() } |
            ForEach-Object { Add-File ("images\" + $story.Name + "\" + $_.Name) $_.FullName }
    }
}

$imageCount = @($files | Where-Object { $_.Rel -like "images\*" }).Count
$totalBytes = ($files | ForEach-Object { (Get-Item -LiteralPath $_.Full).Length } |
    Measure-Object -Sum).Sum

# 剧本清单（下划线开头的模板不算"故事"）
$storyNames = @($files |
    Where-Object { $_.Rel -like "stories\*" } |
    ForEach-Object { [System.IO.Path]::GetFileNameWithoutExtension($_.Rel) } |
    Where-Object { -not $_.StartsWith("_") } |
    Sort-Object)

# ---------- 4. 算新版本号 ----------

# 4.1 两个指纹：代码（引擎）和内容（剧本 + 图片），只用来在日志里注明这版动了什么
$fpCode = (Get-FileHash -LiteralPath $mainSrc -Algorithm SHA256).Hash.Substring(0, 10) + "-" +
          (Get-FileHash -LiteralPath $stubSrc -Algorithm SHA256).Hash.Substring(0, 10)
$sigText = ($files | Sort-Object Rel | ForEach-Object {
        "$($_.Rel):" + (Get-FileHash -LiteralPath $_.Full -Algorithm SHA256).Hash
    }) -join "|"
$fpContent = Get-StringHash $sigText

# 4.2 读日志：手填的大版本号 + 上一条版本记录
$manualMajor = 0
$prevVersion = ""
$prevStories = @()
$prevCode = ""
if (Test-Path -LiteralPath $logPath) {
    $logLines = Get-Content -LiteralPath $logPath -Encoding UTF8

    $majorLine = @($logLines | Where-Object { $_ -match '^\s*大版本\s*[:：]?\s*(\d+)' } |
        Select-Object -Last 1)
    if ($majorLine.Count -gt 0) {
        $null = $majorLine[0] -match '^\s*大版本\s*[:：]?\s*(\d+)'
        $manualMajor = [int]$Matches[1]
    }

    $entries = @($logLines | Where-Object { $_ -match '^\s*[vV]?\d+(\.\d+){0,2}\s*\|' })
    if ($entries.Count -gt 0) {
        $parts = $entries[-1] -split '\|'
        if ($parts.Count -ge 1) { $prevVersion = $parts[0].Trim() }
        if ($parts.Count -ge 5) {
            $prevStories = @($parts[4] -split '[、,]' |
                ForEach-Object { $_.Trim() } |
                Where-Object { $_ -ne "" -and $_ -ne "-" } |
                Sort-Object)
        }
        if ($parts.Count -ge 6) { $prevCode = ($parts[5] -replace '^\s*代码\s*', '').Trim() }
    }
}

$added = @($storyNames | Where-Object { $prevStories -notcontains $_ })
$removed = @($prevStories | Where-Object { $storyNames -notcontains $_ })
$codeChanged = ($prevCode -ne "") -and ($prevCode -ne $fpCode)

function Read-Version([string]$text) {
    $parts = ($text -replace '^[vV]', '') -split '\.'
    $nums = @(0, 0, 0)
    for ($i = 0; $i -lt 3 -and $i -lt $parts.Count; $i++) {
        $n = 0
        if ([int]::TryParse($parts[$i], [ref]$n)) { $nums[$i] = $n }
    }
    return $nums
}

# 4.3 决定新版本号
$level = ""
$reason = ""
if ($prevVersion -eq "") {
    $newVersion = "v$manualMajor.0.0"
    $level = "大版本"
    $reason = "首个版本"
}
else {
    $v = Read-Version $prevVersion
    $prevMajor = $v[0]; $min = $v[1]; $pat = $v[2]

    if ($manualMajor -ne $prevMajor) {
        # 你手动改了大版本号：从 v几.0.0 重新开始
        $newVersion = "v$manualMajor.0.0"
        $level = "大版本"
        $reason = "大版本号由 $prevMajor 改为 $manualMajor"
    }
    elseif ($added.Count -gt 0 -or $removed.Count -gt 0) {
        $min++
        $pat = 0
        $newVersion = "v$prevMajor.$min.$pat"
        $level = "中版本"
        $bits = @()
        if ($added.Count -gt 0) { $bits += "新增剧本：" + ($added -join "、") }
        if ($removed.Count -gt 0) { $bits += "移除剧本：" + ($removed -join "、") }
        $reason = $bits -join "；"
    }
    else {
        $pat++
        $newVersion = "v$prevMajor.$min.$pat"
        $level = "小版本"
        if ($codeChanged) { $reason = "代码 / 引擎有改动" }
        else { $reason = "剧本内容 / 图片有更新" }
    }
}

# ---------- 5. 写版本文件 + 记日志 ----------
$stamp = Get-Date -Format "yyyy-MM-dd HH:mm"
$storyListText = if ($storyNames.Count -gt 0) { $storyNames -join "、" } else { "-" }

$versionText = @(
    $newVersion
    "$stamp · $level（$reason） · 剧本 $($storyNames.Count) 个 · 图片 $imageCount 张"
) -join "`r`n"
[System.IO.File]::WriteAllText($versionFile, $versionText + "`r`n",
    (New-Object System.Text.UTF8Encoding($true)))
Add-File "版本.txt" $versionFile

$logLine = "$newVersion | $stamp | $level（$reason） | 剧本 $($storyNames.Count) · 图片 $imageCount | $storyListText | 代码 $fpCode | 内容 $fpContent"
Add-Content -LiteralPath $logPath -Value $logLine -Encoding UTF8

Info ""
Info "版本：$newVersion（$level）"
Write-Host "  变化：$reason"
if ($codeChanged) {
    Write-Host "  代码指纹变了：$prevCode → $fpCode" -ForegroundColor DarkGray
}
Write-Host ("  内容：剧本 {0} 个，图片 {1} 张，合计 {2:N0} KB" -f $storyNames.Count, $imageCount,
    ($totalBytes / 1KB))

# ---------- 6. 生成包裹数据 ----------
$payload = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($payload)
$writer.Write([uint32]$files.Count)

foreach ($f in $files) {
    $nameBytes = [System.Text.Encoding]::Unicode.GetBytes($f.Rel)
    $writer.Write([uint32]$nameBytes.Length)
    $writer.Write($nameBytes)
    $data = [System.IO.File]::ReadAllBytes($f.Full)
    $writer.Write([uint64]$data.Length)
    $writer.Write($data)
}
$writer.Flush()
$payloadBytes = $payload.ToArray()
$writer.Dispose()
$payload.Dispose()

# ---------- 7. 启动器 + 数据 + 索引，拼成一个 exe ----------
$stubBytes = [System.IO.File]::ReadAllBytes($stubPath)
$out = New-Object System.IO.MemoryStream
$out.Write($stubBytes, 0, $stubBytes.Length)
$out.Write($payloadBytes, 0, $payloadBytes.Length)
$footer = New-Object System.IO.BinaryWriter($out)
$footer.Write([System.Text.Encoding]::ASCII.GetBytes("FOOLBOX1"))
$footer.Write([uint64]$stubBytes.Length)
$footer.Write([uint64]$payloadBytes.Length)
$footer.Flush()

if ($Name -eq "") { $Name = "愚见者的故事书-$newVersion" }
if ($Name -notmatch '\.exe$') { $Name = $Name + ".exe" }
$outPath = Join-Path $OutDir $Name

if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
[System.IO.File]::WriteAllBytes($outPath, $out.ToArray())
$footer.Dispose()
$out.Dispose()

$finalSize = (Get-Item -LiteralPath $outPath).Length
Info ""
Info "打包完成：$outPath"
Write-Host ("  单个 exe，大小 {0:N0} KB（{1:N2} MB）" -f ($finalSize / 1KB), ($finalSize / 1MB))
Write-Host "  对方双击即可运行；第一次会解包到 %LOCALAPPDATA%\愚见者的故事书\ 然后启动游戏。"
Write-Host "  版本记录：版本日志.txt"
