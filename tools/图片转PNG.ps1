<#
  愚见者的故事书 · 图片工具
  ------------------------------------------------------------------
  作用：把 images 文件夹里不是 png 的图片（jpg / bmp / gif / tif）转成 png，
        文件名（数字）保持不变，故事里 @图片 1 就能直接读到。
        原图不会删掉，会移动到同目录的 _原图\ 里，随时可以拿回来。
        顺带检查有没有不是「数字命名」的图片（这种图程序不会使用）。

  用法（在本文件夹外面打开 PowerShell 也照样能用）：
      powershell -ExecutionPolicy Bypass -File tools\图片转PNG.ps1
      powershell -ExecutionPolicy Bypass -File tools\图片转PNG.ps1 -Archive 雨夜末班车
      powershell -ExecutionPolicy Bypass -File tools\图片转PNG.ps1 -Check     # 只看不转

  注：故事书程序本身在扫描图片时就会自动做这件事，这个脚本是给不想开程序、
      或者想一次性批量处理所有文件夹时用的。
#>

param(
    [string]$Archive = "",
    [switch]$Check
)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$imagesRoot = Join-Path $root "images"

if (-not (Test-Path -LiteralPath $imagesRoot)) {
    Write-Host "找不到图片文件夹：$imagesRoot" -ForegroundColor Yellow
    exit 0
}

if ($Archive -ne "") {
    $target = Join-Path $imagesRoot $Archive
    if (-not (Test-Path -LiteralPath $target)) {
        Write-Host "找不到这个档案的图片文件夹：$target" -ForegroundColor Yellow
        exit 0
    }
    $folders = @(Get-Item -LiteralPath $target)
}
else {
    $folders = @(Get-ChildItem -LiteralPath $imagesRoot -Directory)
}

$convertible = @(".jpg", ".jpeg", ".bmp", ".gif", ".tif", ".tiff")
$converted = 0
$skipped = 0
$oddNames = @()

foreach ($folder in $folders) {
    Write-Host ""
    Write-Host "== $($folder.Name) ==" -ForegroundColor Cyan

    $files = @(Get-ChildItem -LiteralPath $folder.FullName -File)
    if ($files.Count -eq 0) {
        Write-Host "  （空文件夹）" -ForegroundColor DarkGray
        continue
    }

    foreach ($f in $files) {
        $ext = $f.Extension.ToLower()
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)

        if ($stem -notmatch '^\d+$') { $oddNames += "$($folder.Name)\$($f.Name)" }
        if ($ext -eq ".png") { continue }

        if ($convertible -notcontains $ext) {
            Write-Host ("  跳过（不是常见格式，请先另存为 png）：{0}" -f $f.Name) -ForegroundColor DarkYellow
            $skipped++
            continue
        }

        $outPath = Join-Path $folder.FullName ($stem + ".png")
        if (Test-Path -LiteralPath $outPath) {
            Write-Host ("  已存在 {0}.png，跳过 {1}" -f $stem, $f.Name) -ForegroundColor DarkYellow
            $skipped++
            continue
        }

        if ($Check) {
            Write-Host ("  可以转换：{0}  ->  {1}.png" -f $f.Name, $stem) -ForegroundColor Green
            continue
        }

        try {
            $img = [System.Drawing.Image]::FromFile($f.FullName)
            $img.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
            $img.Dispose()

            $backup = Join-Path $folder.FullName "_原图"
            if (-not (Test-Path -LiteralPath $backup)) {
                New-Item -ItemType Directory -Path $backup | Out-Null
            }
            Move-Item -LiteralPath $f.FullName -Destination (Join-Path $backup $f.Name)

            Write-Host ("  已转换：{0}  ->  {1}.png（原图移到 _原图\）" -f $f.Name, $stem) -ForegroundColor Green
            $converted++
        }
        catch {
            Write-Host ("  转换失败：{0} —— {1}" -f $f.Name, $_.Exception.Message) -ForegroundColor Red
            $skipped++
        }
    }
}

Write-Host ""
if ($oddNames.Count -gt 0) {
    Write-Host "注意：下面这些图片不是数字命名，游戏不会读取它们：" -ForegroundColor Yellow
    foreach ($n in $oddNames) { Write-Host "    $n" -ForegroundColor Yellow }
    Write-Host "      想让游戏用到，就把名字改成 1.png、2.png 这样的数字。" -ForegroundColor DarkGray
}
Write-Host ("完成：转换 {0} 个，跳过 {1} 个。" -f $converted, $skipped) -ForegroundColor Cyan
if ($Check) { Write-Host "（这次是 -Check 预览，没有真的改文件。）" -ForegroundColor DarkGray }

if ($Host.Name -eq "ConsoleHost") {
    Write-Host ""
    Read-Host "按回车关闭"
}
