$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $ProjectRoot

function Fail($Message) {
    Write-Host "`n[失败] $Message" -ForegroundColor Red
    Read-Host "按回车关闭"
    exit 1
}

Write-Host "=== Refraction Rift 一键推送 ===" -ForegroundColor Cyan
Write-Host "项目：$ProjectRoot"
Write-Host "请确认 UE 编辑器已经保存并关闭。`n"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "找不到 Git，请先安装 Git for Windows。" }
if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot '.git'))) { Fail "当前目录不是 Git 仓库。" }

try { git lfs install --local | Out-Null } catch { Fail "Git LFS 初始化失败：$($_.Exception.Message)" }

Write-Host "[1/4] 检查并暂存修改..." -ForegroundColor Yellow
git add .

$Staged = git diff --cached --name-only
if ($Staged) {
    $Message = Read-Host "请输入提交说明（直接回车使用默认说明）"
    if ([string]::IsNullOrWhiteSpace($Message)) { $Message = "update: Refraction Rift project" }
    git commit -m $Message
} else {
    Write-Host "没有新的本地修改，跳过提交。" -ForegroundColor DarkGray
}

Write-Host "[2/4] 同步远程 main 分支..." -ForegroundColor Yellow
$RemoteMain = git ls-remote --heads origin main
if ($RemoteMain) {
    try { git pull --rebase origin main } catch { Fail "同步失败。可能存在冲突，请查看上方信息并联系协作者处理。" }
} else {
    Write-Host "远程仓库还没有 main 分支，跳过同步。" -ForegroundColor DarkGray
}

Write-Host "[3/4] 推送到 GitHub..." -ForegroundColor Yellow
try { git push -u origin main } catch { Fail "推送失败。请确认已登录 GitHub，或查看上方错误信息。" }

Write-Host "`n[完成] 项目已经推送到 GitHub。" -ForegroundColor Green
Read-Host "按回车关闭"

