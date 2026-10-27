Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $ProjectRoot

function Get-ProxyArgs {
    $result = @()
    try {
        $settings = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction Stop
        if ($settings.ProxyEnable -eq 1 -and $settings.ProxyServer) {
            $proxy = [string]$settings.ProxyServer
            if ($proxy -match 'https=([^;]+)') { $proxy = $Matches[1] }
            elseif ($proxy -match 'http=([^;]+)') { $proxy = $Matches[1] }
            if ($proxy -notmatch '^https?://') { $proxy = "http://$proxy" }
            $result = @('-c', "http.proxy=$proxy", '-c', "https.proxy=$proxy")
        }
    } catch {}
    return $result
}

$GitProxyArgs = Get-ProxyArgs
function Invoke-Git([string[]]$Arguments) {
    $lines = @(& git @GitProxyArgs @Arguments 2>&1)
    [pscustomobject]@{ Code = $LASTEXITCODE; Text = ($lines -join [Environment]::NewLine) }
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Refraction Rift - Git 同步工具'
$form.Size = New-Object System.Drawing.Size(760, 560)
$form.StartPosition = 'CenterScreen'
$form.MinimumSize = New-Object System.Drawing.Size(680, 460)

$title = New-Object System.Windows.Forms.Label
$title.Text = 'Refraction Rift  项目同步'
$title.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 16, [System.Drawing.FontStyle]::Bold)
$title.AutoSize = $true
$title.Location = New-Object System.Drawing.Point(20, 18)
$form.Controls.Add($title)

$pathLabel = New-Object System.Windows.Forms.Label
$pathLabel.Text = "项目：$ProjectRoot"
$pathLabel.AutoSize = $true
$pathLabel.Location = New-Object System.Drawing.Point(22, 54)
$form.Controls.Add($pathLabel)

$statusLabel = New-Object System.Windows.Forms.Label
$statusLabel.Text = '状态：检查中...'
$statusLabel.AutoSize = $true
$statusLabel.Location = New-Object System.Drawing.Point(22, 78)
$form.Controls.Add($statusLabel)

$proxyLabel = New-Object System.Windows.Forms.Label
$proxyLabel.Text = if($GitProxyArgs.Count -gt 0){ '代理：已检测并使用系统代理' } else { '代理：未检测到系统代理' }
$proxyLabel.AutoSize = $true
$proxyLabel.Location = New-Object System.Drawing.Point(22, 102)
$form.Controls.Add($proxyLabel)

$msgLabel = New-Object System.Windows.Forms.Label
$msgLabel.Text = '提交说明：'
$msgLabel.AutoSize = $true
$msgLabel.Location = New-Object System.Drawing.Point(22, 138)
$form.Controls.Add($msgLabel)

$msgBox = New-Object System.Windows.Forms.TextBox
$msgBox.Text = 'update: Refraction Rift project'
$msgBox.Location = New-Object System.Drawing.Point(90, 134)
$msgBox.Size = New-Object System.Drawing.Size(390, 24)
$form.Controls.Add($msgBox)

$btnRefresh = New-Object System.Windows.Forms.Button
$btnRefresh.Text = '刷新状态'
$btnRefresh.Location = New-Object System.Drawing.Point(500, 132)
$btnRefresh.Size = New-Object System.Drawing.Size(100, 30)
$form.Controls.Add($btnRefresh)

$btnOpen = New-Object System.Windows.Forms.Button
$btnOpen.Text = '打开项目目录'
$btnOpen.Location = New-Object System.Drawing.Point(610, 132)
$btnOpen.Size = New-Object System.Drawing.Size(115, 30)
$form.Controls.Add($btnOpen)

$btnPull = New-Object System.Windows.Forms.Button
$btnPull.Text = '拉取更新'
$btnPull.Location = New-Object System.Drawing.Point(22, 176)
$btnPull.Size = New-Object System.Drawing.Size(150, 38)
$form.Controls.Add($btnPull)

$btnPush = New-Object System.Windows.Forms.Button
$btnPush.Text = '推送修改'
$btnPush.Location = New-Object System.Drawing.Point(184, 176)
$btnPush.Size = New-Object System.Drawing.Size(150, 38)
$form.Controls.Add($btnPush)

$hint = New-Object System.Windows.Forms.Label
$hint.Text = '使用前保存并关闭 UE。拉取前如果存在本地未提交修改，工具会停止保护文件。'
$hint.AutoSize = $true
$hint.Location = New-Object System.Drawing.Point(350, 188)
$form.Controls.Add($hint)

$log = New-Object System.Windows.Forms.RichTextBox
$log.ReadOnly = $true
$log.BackColor = [System.Drawing.Color]::FromArgb(30, 30, 30)
$log.ForeColor = [System.Drawing.Color]::White
$log.Font = New-Object System.Drawing.Font('Consolas', 10)
$log.Location = New-Object System.Drawing.Point(22, 232)
$log.Size = New-Object System.Drawing.Size(703, 270)
$log.Anchor = 'Top,Bottom,Left,Right'
$form.Controls.Add($log)

function Write-Log($Text, $Color = [System.Drawing.Color]::White) {
    $log.SelectionStart = $log.TextLength
    $log.SelectionColor = $Color
    $log.AppendText("$Text`r`n")
    $log.ScrollToCaret()
}
function Set-Busy($Busy) {
    $btnPull.Enabled = -not $Busy
    $btnPush.Enabled = -not $Busy
    $btnRefresh.Enabled = -not $Busy
    $btnOpen.Enabled = -not $Busy
    $form.Cursor = if($Busy){ [System.Windows.Forms.Cursors]::WaitCursor } else { [System.Windows.Forms.Cursors]::Default }
}
function Refresh-Status {
    $r = Invoke-Git @('status','-sb')
    if ($r.Code -eq 0) {
        $first = ($r.Text -split "`r?`n")[0]
        $statusLabel.Text = "状态：$first"
        Write-Log $r.Text ([System.Drawing.Color]::LightGray)
    } else {
        $statusLabel.Text = '状态：无法读取 Git 状态'
        Write-Log $r.Text ([System.Drawing.Color]::OrangeRed)
    }
}

$btnOpen.Add_Click({ Start-Process explorer.exe -ArgumentList "`"$ProjectRoot`"" })
$btnRefresh.Add_Click({ Refresh-Status })
$btnPull.Add_Click({
    Set-Busy $true
    Write-Log '--- 开始拉取 ---' ([System.Drawing.Color]::DeepSkyBlue)
    $dirty = Invoke-Git @('status','--porcelain')
    if ($dirty.Text.Trim()) {
        Write-Log '检测到本地未提交修改，已停止拉取，避免覆盖文件。' ([System.Drawing.Color]::Orange)
    } else {
        $r = Invoke-Git @('pull','--rebase','origin','main')
        Write-Log $r.Text
        if ($r.Code -eq 0) {
            $r2 = Invoke-Git @('lfs','pull')
            Write-Log $r2.Text
            if ($r2.Code -eq 0) { Write-Log '拉取完成。' ([System.Drawing.Color]::LimeGreen) }
            else { Write-Log 'Git LFS 拉取失败。' ([System.Drawing.Color]::OrangeRed) }
        } else { Write-Log 'Git 拉取失败，请检查网络或代理。' ([System.Drawing.Color]::OrangeRed) }
    }
    Set-Busy $false
    Refresh-Status
})
$btnPush.Add_Click({
    Set-Busy $true
    Write-Log '--- 开始推送 ---' ([System.Drawing.Color]::DeepSkyBlue)
    $r = Invoke-Git @('add','.')
    if ($r.Code -ne 0) { Write-Log $r.Text ([System.Drawing.Color]::OrangeRed); Set-Busy $false; return }
    $staged = Invoke-Git @('diff','--cached','--name-only')
    if ($staged.Text.Trim()) {
        $message = $msgBox.Text.Trim()
        if (-not $message) { $message = 'update: Refraction Rift project' }
        $r = Invoke-Git @('commit','-m',$message)
        Write-Log $r.Text
        if ($r.Code -ne 0) { Write-Log '提交失败。' ([System.Drawing.Color]::OrangeRed); Set-Busy $false; return }
    } else { Write-Log '没有新的本地修改，跳过提交。' ([System.Drawing.Color]::Gray) }
    $remote = Invoke-Git @('ls-remote','--heads','origin','main')
    if ($remote.Code -eq 0 -and $remote.Text.Trim()) {
        $r = Invoke-Git @('pull','--rebase','origin','main')
        Write-Log $r.Text
        if ($r.Code -ne 0) { Write-Log '同步失败，可能存在冲突。' ([System.Drawing.Color]::OrangeRed); Set-Busy $false; return }
    }
    $r = Invoke-Git @('push','-u','origin','main')
    Write-Log $r.Text
    if ($r.Code -eq 0) { Write-Log '推送完成。' ([System.Drawing.Color]::LimeGreen) }
    else { Write-Log '推送失败，请检查网络或代理。' ([System.Drawing.Color]::OrangeRed) }
    Set-Busy $false
    Refresh-Status
})

Write-Log "项目路径：$ProjectRoot" ([System.Drawing.Color]::LightGray)
Write-Log '请保存并关闭 UE 后再进行同步。' ([System.Drawing.Color]::Gold)
Refresh-Status
[void]$form.ShowDialog()