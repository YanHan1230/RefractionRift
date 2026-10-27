param([switch]$NoUI, [switch]$SmokeTest, [string]$ProjectPath)

function Resolve-GitPath {
    $command = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
        if (-not $base) { continue }
        foreach ($relative in @('Git\cmd\git.exe', 'Programs\Git\cmd\git.exe')) {
            $candidate = Join-Path $base $relative
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }
    throw '未找到 Git。请安装 Git for Windows 和 Git LFS，然后重新打开同步工具。'
}

if (-not ('RiftSync.LiveGit' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
namespace RiftSync {
    public class Event { public string Kind; public string Text; public Event(string kind, string text) { Kind=kind; Text=text; } }
    public class Result { public int Code; public string Text; }
    public static class LiveGit {
        public static void Emit(ConcurrentQueue<Event> queue, string kind, string text) { queue.Enqueue(new Event(kind,text)); }
        public static string Quote(string value) {
            if(value.Length>0 && value.IndexOfAny(new char[]{' ', '\t', '\n', '\v', '"'})<0) return value;
            var b=new StringBuilder("\""); int slashes=0;
            foreach(char c in value) {
                if(c=='\\') { slashes++; continue; }
                if(c=='"') { b.Append('\\',slashes*2+1); b.Append(c); slashes=0; continue; }
                b.Append('\\',slashes); slashes=0; b.Append(c);
            }
            b.Append('\\',slashes*2); b.Append('"'); return b.ToString();
        }
        public static Result Run(string git, string root, string[] prefix, string[] args,
            ConcurrentQueue<Event> queue, string logFile, string progressFile) {
            var all=new List<string>(); all.AddRange(prefix); all.Add("-C"); all.Add(root); all.AddRange(args);
            var quoted=new List<string>(); foreach(string a in all) quoted.Add(Quote(a));
            var info=new ProcessStartInfo(git,string.Join(" ",quoted.ToArray()));
            info.WorkingDirectory=root; info.UseShellExecute=false; info.CreateNoWindow=true;
            info.RedirectStandardOutput=true; info.RedirectStandardError=true;
            info.StandardOutputEncoding=new UTF8Encoding(false); info.StandardErrorEncoding=new UTF8Encoding(false);
            info.EnvironmentVariables["GIT_LFS_FORCE_PROGRESS"]="1";
            info.EnvironmentVariables["GIT_PROGRESS_DELAY"]="0";
            info.EnvironmentVariables["GIT_TERMINAL_PROMPT"]="0";
            if(!string.IsNullOrEmpty(progressFile)) info.EnvironmentVariables["GIT_LFS_PROGRESS"]=progressFile;
            var output=new ConcurrentQueue<string>(); var gate=new object();
            DataReceivedEventHandler received=(sender,e)=> {
                if(e.Data==null || e.Data.Length==0) return;
                output.Enqueue(e.Data); Emit(queue,"output",e.Data);
                if(!string.IsNullOrEmpty(logFile)) lock(gate) {
                    try { File.AppendAllText(logFile,DateTime.Now.ToString("HH:mm:ss ")+e.Data+Environment.NewLine,new UTF8Encoding(false)); } catch {}
                }
            };
            using(var process=new Process()) {
                process.StartInfo=info; process.OutputDataReceived+=received; process.ErrorDataReceived+=received;
                process.Start(); process.BeginOutputReadLine(); process.BeginErrorReadLine();
                process.WaitForExit();
                return new Result { Code=process.ExitCode, Text=string.Join(Environment.NewLine,output.ToArray()) };
            }
        }
    }
}
'@
}

$SyncWorker = {
    param($Root, $Mode, $Message, $Git, [string[]]$ProxyArgs, $Queue, $LogFile, $ProgressFile)
    $ErrorActionPreference = 'Stop'
    $prefix = @('-c','core.quotepath=false') + @($ProxyArgs)
    function Emit([string]$Kind,[string]$Text) { [RiftSync.LiveGit]::Emit($Queue,$Kind,$Text) }
    function Run-Git([string[]]$GitArgs) {
        [RiftSync.LiveGit]::Run($Git,$Root,[string[]]$prefix,$GitArgs,$Queue,$LogFile,$ProgressFile)
    }
    function Step([string]$Label,[string[]]$GitArgs) {
        Emit 'stage' $Label
        $r = Run-Git $GitArgs
        if ($r.Code -ne 0) { throw "$Label 失败（退出码 $($r.Code)），详细原因请看日志。" }
        return $r
    }
    function Sync-FromOrigin([string]$Branch) {
        $r = Step '获取主仓库更新' @('fetch','--progress','origin')
        $remoteBranch = Run-Git @('show-ref','--verify','--quiet',"refs/remotes/origin/$Branch")
        if ($remoteBranch.Code -eq 0) {
            $r = Step '合并当前分支的远端更新' @('merge','--no-edit',"origin/$Branch")
        } elseif ($remoteBranch.Code -ne 1) { throw '无法检查远端分支，已停止同步。' }
        if ($Branch -ne 'main') {
            $r = Step '合并主分支更新' @('merge','--no-edit','origin/main')
        }
    }
    try {
        if ($Mode -eq 'status') {
            Emit 'stage' '读取项目状态'
            $r = Run-Git @('status','--short','--branch')
            if ($r.Code -ne 0) { throw '无法读取 Git 状态。' }
            Emit 'status' (($r.Text -split '\r?\n' | Where-Object { $_ -match '^## ' } | Select-Object -First 1) -replace '^## ','')
            Emit 'success' '状态刷新完成'
            return
        }
        if (Test-Path -LiteralPath (Join-Path $Root '.git/index.lock')) { throw '另一个 Git 操作正在使用项目，请等待它结束。' }
        $branchResult = Step '检查当前工作分支' @('symbolic-ref','--quiet','--short','HEAD')
        $branch = $branchResult.Text.Trim()
        if ($Mode -eq 'push' -and $branch -in @('main','master')) {
            throw '请先切换到自己的 feature 分支，再推送修改并创建 Pull Request。'
        }
        if ($Mode -eq 'pull') {
            Emit 'stage' '检查本地修改'
            $dirty = Run-Git @('status','--porcelain')
            if ($dirty.Code -ne 0) { throw '无法检查本地修改，已停止拉取。' }
            if ($dirty.Text.Trim()) { throw '存在未提交的本地修改，请先推送或保存修改后再拉取。' }
            Sync-FromOrigin $branch
            $r = Step '下载大文件（Git LFS）' @('lfs','pull')
        } elseif ($Mode -eq 'push') {
            $r = Step '整理修改并准备大文件' @('add','--all')
            Emit 'stage' '检查待提交文件'
            $changes = Run-Git @('diff','--cached','--quiet')
            if ($changes.Code -eq 1) {
                $r = Step '创建本地提交' @('commit','-m',$Message)
            } elseif ($changes.Code -eq 0) {
                Emit 'info' '没有新的文件修改，继续检查待上传提交。'
            } else { throw '无法读取待提交内容。' }
            Sync-FromOrigin $branch
            $r = Step '上传大文件与提交到当前分支' @('push','--progress','-u','origin',$branch)
            $head = Step '核对上传结果' @('rev-parse','HEAD')
            $remote = Step '核对 GitHub 当前分支' @('ls-remote','--heads','origin',"refs/heads/$branch")
            $localHash = ($head.Text -split '\r?\n' | Where-Object { $_ -match '^[a-f0-9]{40,64}$' } | Select-Object -First 1)
            $remoteRefPattern = '^[a-f0-9]{40,64}\s+' + [regex]::Escape("refs/heads/$branch") + '\s*$'
            $remoteHash = ($remote.Text -split '\r?\n' | Where-Object { $_ -match $remoteRefPattern } | Select-Object -First 1) -split '\s+'
            if (-not $localHash -or -not $remoteHash -or $remoteHash[0] -ne $localHash) { throw '远端提交与本地不一致，暂不能确认推送完成，请检查日志。' }
        } else { throw "未知操作：$Mode" }
        Emit 'stage' '读取同步后的状态'
        $status = Run-Git @('status','--short','--branch')
        if ($status.Code -ne 0) { throw '传输已结束，但读取项目状态失败，请刷新状态。' }
        Emit 'status' (($status.Text -split '\r?\n' | Where-Object { $_ -match '^## ' } | Select-Object -First 1) -replace '^## ','')
        if ($Mode -eq 'push') { Emit 'success' '推送完成，已核对 GitHub 与本地提交一致' }
        else { Emit 'success' '拉取完成，大文件下载已结束' }
    } catch { Emit 'failed' $_.Exception.Message }
}

function Get-TransferProgress([string]$Line) {
    if ($Line -match '(Uploading|Downloading) LFS objects:\s*(\d{1,3})%\s*\((\d+)/(\d+)\)(.*)') {
        $direction = if ($Matches[1] -eq 'Uploading') { '上传大文件' } else { '下载大文件' }
        return [pscustomobject]@{ Percent=[math]::Min(100,[int]$Matches[2]); Text="$direction $($Matches[2])%  ·  $($Matches[3])/$($Matches[4]) 个文件$($Matches[5])" }
    }
    if ($Line -match '(Enumerating|Counting|Compressing|Writing|Receiving|Resolving|Updating|Checking out)(?: objects| deltas| files)?:\s*(\d{1,3})%(.*)') {
        $names=@{Enumerating='扫描文件';Counting='统计对象';Compressing='压缩数据';Writing='上传提交';Receiving='接收提交';Resolving='整理差异';Updating='更新文件';'Checking out'='写入文件'}
        return [pscustomobject]@{ Percent=[math]::Min(100,[int]$Matches[2]); Text="$($names[$Matches[1]]) $($Matches[2])%$($Matches[3])" }
    }
    return $null
}

if ($NoUI) { return }
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
if (-not $ProjectPath) { $ProjectPath = Split-Path -Parent $MyInvocation.MyCommand.Path }
$ProjectRoot = [IO.Path]::GetFullPath($ProjectPath)
$ToolPath = $MyInvocation.MyCommand.Path
try { $GitPath = Resolve-GitPath }
catch {
    [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message, '项目同步', 'OK', 'Error')
    return
}
$GitProxyArgs = @()
try {
    $settings=Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction Stop
    if ($settings.ProxyEnable -eq 1 -and $settings.ProxyServer) {
        $proxy=[string]$settings.ProxyServer
        if ($proxy -match 'https=([^;]+)') { $proxy=$Matches[1] }
        elseif ($proxy -match 'http=([^;]+)') { $proxy=$Matches[1] }
        if ($proxy -notmatch '^https?://') { $proxy="http://$proxy" }
        $GitProxyArgs=@('-c',"http.proxy=$proxy",'-c',"https.proxy=$proxy")
    }
} catch {}

$form=New-Object System.Windows.Forms.Form
$form.Text='Refraction Rift - 实时同步工具'
$form.Size=New-Object System.Drawing.Size(900,700)
$form.MinimumSize=New-Object System.Drawing.Size(850,620)
$form.StartPosition='CenterScreen'
$form.Font=New-Object System.Drawing.Font('Microsoft YaHei UI',9)
function Label($Text,$X,$Y,$Width) {
    $c=New-Object System.Windows.Forms.Label
    $c.Text=$Text; $c.Location=New-Object System.Drawing.Point($X,$Y); $c.Size=New-Object System.Drawing.Size($Width,24)
    $form.Controls.Add($c); return $c
}
$title=Label 'Refraction Rift  项目同步' 22 18 800
$title.Font=New-Object System.Drawing.Font('Microsoft YaHei UI',16,[System.Drawing.FontStyle]::Bold)
$title.Height=34
$pathLabel=Label "项目：$ProjectRoot" 24 58 820
$statusLabel=Label '版本状态：尚未刷新' 24 86 820
$proxyLabel=Label $(if($GitProxyArgs.Count){'代理：使用系统代理'}else{'代理：直接连接'}) 24 112 820
$messageLabel=Label '提交说明：' 24 151 78
$msgBox=New-Object System.Windows.Forms.TextBox
$msgBox.Text='update: Refraction Rift project'; $msgBox.Location=New-Object System.Drawing.Point(102,146); $msgBox.Size=New-Object System.Drawing.Size(470,28)
$form.Controls.Add($msgBox)
function Button($Text,$X,$Y,$Width) {
    $c=New-Object System.Windows.Forms.Button; $c.Text=$Text
    $c.Location=New-Object System.Drawing.Point($X,$Y); $c.Size=New-Object System.Drawing.Size($Width,36)
    $form.Controls.Add($c); return $c
}
$btnPull=Button '拉取更新' 24 190 145
$btnPush=Button '推送修改' 181 190 145
$btnRefresh=Button '刷新状态' 338 190 130
$btnOpen=Button '打开项目目录' 480 190 150
$btnLog=Button '查看日志' 642 190 120
$stageLabel=Label '准备就绪' 24 242 820
$stageLabel.Font=New-Object System.Drawing.Font('Microsoft YaHei UI',10,[System.Drawing.FontStyle]::Bold)
$progress=New-Object System.Windows.Forms.ProgressBar
$progress.Location=New-Object System.Drawing.Point(24,272); $progress.Size=New-Object System.Drawing.Size(830,22); $progress.Anchor='Top,Left,Right'
$form.Controls.Add($progress)
$detailLabel=Label '有明确进度时显示百分比，其余步骤显示运行状态。' 24 303 830
$timeLabel=Label '已用时间 00:00:00' 24 333 830
$hint=Label '同步前保存并关闭 UE。上传期间窗口可正常刷新，请勿同时启动其他同步工具。' 24 363 830
$hint.ForeColor=[System.Drawing.Color]::DimGray
$log=New-Object System.Windows.Forms.RichTextBox
$log.Location=New-Object System.Drawing.Point(24,398); $log.Size=New-Object System.Drawing.Size(830,245)
$log.Anchor='Top,Bottom,Left,Right'; $log.ReadOnly=$true; $log.WordWrap=$false
$log.BackColor=[System.Drawing.Color]::FromArgb(28,31,37); $log.ForeColor=[System.Drawing.Color]::WhiteSmoke
$log.Font=New-Object System.Drawing.Font('Consolas',9)
$form.Controls.Add($log)
$script:Operation=$null; $script:LatestLog=$null
function Write-Log([string]$Text,$Color=[System.Drawing.Color]::WhiteSmoke) {
    if ($log.TextLength -gt 240000) { $log.Select(0,80000); $log.SelectedText='' }
    $log.SelectionStart=$log.TextLength; $log.SelectionColor=$Color
    $log.AppendText(('[{0}] {1}' -f (Get-Date -Format HH:mm:ss),$Text)+"`r`n"); $log.ScrollToCaret()
}
function Set-Busy([bool]$Busy) {
    $btnPull.Enabled=-not $Busy; $btnPush.Enabled=-not $Busy; $btnRefresh.Enabled=-not $Busy; $msgBox.Enabled=-not $Busy
}
function Start-Sync([string]$Mode) {
    if ($script:Operation) { return }
    if ($Mode -ne 'status') {
        $others=@(Get-CimInstance Win32_Process -Filter "Name='powershell.exe' OR Name='pwsh.exe'" -ErrorAction SilentlyContinue | Where-Object {
            $_.ProcessId -ne $PID -and $_.CommandLine -and $_.CommandLine.IndexOf($ToolPath,[StringComparison]::OrdinalIgnoreCase) -ge 0 -and $_.CommandLine -match '(?i)-file\s'
        })
        if ($others.Count) { Write-Log '另一个同步工具仍在打开。请等待原推送结束并关闭旧窗口，再使用本窗口。' ([System.Drawing.Color]::Orange); return }
    }
    $logDir=Join-Path $ProjectRoot '.git/sync-tool'
    [void][IO.Directory]::CreateDirectory($logDir)
    $id=(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$PID
    $script:LatestLog=Join-Path $logDir ($id+'.log')
    $progressFile=Join-Path $logDir ($id+'.lfs-progress')
    $queue=New-Object 'System.Collections.Concurrent.ConcurrentQueue[RiftSync.Event]'
    $ps=[PowerShell]::Create()
    $message=$msgBox.Text.Trim(); if (-not $message) { $message='update: Refraction Rift project' }
    [void]$ps.AddScript($SyncWorker.ToString()).AddArgument($ProjectRoot).AddArgument($Mode).AddArgument($message).AddArgument($GitPath).AddArgument([string[]]$GitProxyArgs).AddArgument($queue).AddArgument($script:LatestLog).AddArgument($progressFile)
    $script:Operation=@{ PS=$ps; Async=$ps.BeginInvoke(); Queue=$queue; Watch=[Diagnostics.Stopwatch]::StartNew(); LastOutput=[datetime]::Now; LastLine='等待 Git 输出'; Terminal=$false; Outcome=$null; ProgressFile=$progressFile }
    Set-Busy $true; $progress.Value=0; $progress.Style='Marquee'
    $stageLabel.Text='正在启动'; $stageLabel.ForeColor=[System.Drawing.Color]::Black
    $detailLabel.Text='后台任务已启动，窗口会持续刷新。'
    Write-Log "开始操作：$Mode" ([System.Drawing.Color]::DeepSkyBlue)
}
$timer=New-Object System.Windows.Forms.Timer
$timer.Interval=150
$timer.Add_Tick({
    $op=$script:Operation
    if (-not $op) { return }
    $elapsed=$op.Watch.Elapsed
    $silence=[int]([datetime]::Now-$op.LastOutput).TotalSeconds
    $timeLabel.Text=('已用时间 {0:00}:{1:00}:{2:00}   ·   距上次输出 {3} 秒' -f [int][math]::Floor($elapsed.TotalHours),$elapsed.Minutes,$elapsed.Seconds,$silence)
    $event=$null; $drained=0
    while ($drained -lt 300 -and $op.Queue.TryDequeue([ref]$event)) {
        $drained++; $op.LastOutput=[datetime]::Now
        switch ($event.Kind) {
            'stage' { $stageLabel.Text=$event.Text; $progress.Style='Marquee'; $detailLabel.Text='当前步骤进行中，等待 Git 报告进度。'; Write-Log $event.Text ([System.Drawing.Color]::DeepSkyBlue) }
            'status' { $statusLabel.Text='版本状态：'+$event.Text }
            'output' {
                $p=Get-TransferProgress $event.Text
                if ($p) {
                    $progress.Style='Continuous'; $progress.Value=$p.Percent; $detailLabel.Text=$p.Text
                    if ($event.Text -ne $op.LastLine) { Write-Log $event.Text ([System.Drawing.Color]::LightSkyBlue) }
                } else { Write-Log $event.Text }
                $op.LastLine=$event.Text
            }
            'info' { Write-Log $event.Text ([System.Drawing.Color]::LightGray) }
            'success' { $op.Terminal=$true; $op.Outcome='success'; $stageLabel.Text=$event.Text; $stageLabel.ForeColor=[System.Drawing.Color]::ForestGreen; $progress.Style='Continuous'; $progress.Value=100; $detailLabel.Text='操作完成'; Write-Log $event.Text ([System.Drawing.Color]::LightGreen) }
            'failed' { $op.Terminal=$true; $op.Outcome='failed'; $stageLabel.Text='操作未完成'; $stageLabel.ForeColor=[System.Drawing.Color]::Firebrick; $progress.Style='Continuous'; $progress.Value=0; $detailLabel.Text=$event.Text; Write-Log $event.Text ([System.Drawing.Color]::OrangeRed) }
        }
    }
    if (-not $op.Terminal -and $silence -ge 20) { $detailLabel.Text="当前步骤已有 $silence 秒没有新输出，后台任务尚未结束；可查看日志。" }
    if ($op.Async.IsCompleted -and $op.Queue.IsEmpty) {
        try { [void]$op.PS.EndInvoke($op.Async); if ($op.PS.HadErrors) { throw ($op.PS.Streams.Error | Out-String) }; if (-not $op.Terminal) { throw '任务意外结束，请查看日志。' } }
        catch { $stageLabel.Text='操作未完成'; $stageLabel.ForeColor=[System.Drawing.Color]::Firebrick; $progress.Style='Continuous'; $progress.Value=0; $detailLabel.Text=$_.Exception.Message; Write-Log $_.Exception.Message ([System.Drawing.Color]::OrangeRed) }
        finally { $op.Watch.Stop(); $op.PS.Dispose(); $script:Operation=$null; Set-Busy $false }
    }
})
$btnPush.Add_Click({ Start-Sync 'push' })
$btnPull.Add_Click({ Start-Sync 'pull' })
$btnRefresh.Add_Click({ Start-Sync 'status' })
$btnOpen.Add_Click({ Start-Process explorer.exe -ArgumentList ('"'+$ProjectRoot+'"') })
$btnLog.Add_Click({ if ($script:LatestLog -and (Test-Path -LiteralPath $script:LatestLog)) { Start-Process notepad.exe -ArgumentList ('"'+$script:LatestLog+'"') } else { Write-Log '暂时没有日志。' } })
$form.Add_FormClosing({
    if ($script:Operation) { $_.Cancel=$true; Write-Log '任务仍在执行，请等待完成后关闭窗口。最小化窗口不会中断同步。' ([System.Drawing.Color]::Orange) }
})
$form.Add_Shown({
    $timer.Start()
    if ($SmokeTest) {
        $script:SmokeTimer=New-Object System.Windows.Forms.Timer; $script:SmokeTimer.Interval=1200
        $script:SmokeTimer.Add_Tick({ $script:SmokeTimer.Stop(); $form.Close() }); $script:SmokeTimer.Start()
    } else { Start-Sync 'status' }
})
Write-Log '实时日志已启用。百分比只表示当前步骤，不代表整个同步流程。' ([System.Drawing.Color]::Gold)
[void]$form.ShowDialog()
$timer.Stop(); $timer.Dispose(); $form.Dispose()
