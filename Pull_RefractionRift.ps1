$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $ProjectRoot

function Fail($Message) {
    Write-Host "`n[停止] $Message" -ForegroundColor Red
    Read-Host "按回车关闭"
    exit 1
}

Write-Host "=== Refraction Rift 一键拉取 ===" -ForegroundColor Cyan
Write-Host "项目：$ProjectRoot`n"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "找不到 Git。" }
if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot ".git"))) { Fail "当前目录不是 Git 仓库，请先完成第一次克隆。" }
if ((git status --porcelain)) { Fail "本地存在未提交修改。请先提交或备份后再拉取。" }

Write-Host "[1/2] 拉取 GitHub 最新提交..." -ForegroundColor Yellow
& git pull --rebase origin main
if ($LASTEXITCODE -ne 0) { Fail "Git 拉取失败，请检查网络或登录状态。" }

Write-Host "[2/2] 拉取 Git LFS 资源..." -ForegroundColor Yellow
& git lfs pull
if ($LASTEXITCODE -ne 0) { Fail "Git LFS 拉取失败，请确认 Git LFS 已安装。" }

Write-Host "`n[完成] 项目已经更新到最新版本。" -ForegroundColor Green
Read-Host "按回车关闭"