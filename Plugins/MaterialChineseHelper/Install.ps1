param([string]$ProjectFile)
$ErrorActionPreference = 'Stop'

try {
    if ([string]::IsNullOrWhiteSpace($ProjectFile)) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = '选择要安装材质中文助手的 UE 工程'
        $dialog.Filter = 'Unreal Engine 工程 (*.uproject)|*.uproject'
        $dialog.Multiselect = $false
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
            Write-Host '已取消，没有修改工程。'
            exit 0
        }
        $ProjectFile = $dialog.FileName
    }

    $resolvedProject = (Resolve-Path -LiteralPath $ProjectFile).Path
    if ([IO.Path]::GetExtension($resolvedProject) -ne '.uproject') {
        throw '请选择 .uproject 工程文件。'
    }
    $projectRoot = Split-Path -Parent $resolvedProject
    $pluginSource = [IO.Path]::GetFullPath($PSScriptRoot)
    $pluginTarget = [IO.Path]::GetFullPath((Join-Path $projectRoot 'Plugins\MaterialChineseHelper'))
    $pluginsRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'Plugins'))
    if ((Split-Path -Parent $pluginTarget) -ne $pluginsRoot) {
        throw '无法确认安装目录，请使用说明中的手动安装方式。'
    }
    if ($pluginTarget.StartsWith($pluginSource + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        $pluginSource.Equals($pluginTarget, [StringComparison]::OrdinalIgnoreCase)) {
        throw '安装目标位于插件自身目录内，请选择独立的 UE 工程。'
    }

    $normalizedProject = $resolvedProject.Replace('\', '/')
    $runningEditors = Get-CimInstance Win32_Process -Filter "Name='UnrealEditor.exe' OR Name='UnrealEditor-Cmd.exe'"
    foreach ($editorProcess in $runningEditors) {
        if ($editorProcess.CommandLine -and
            $editorProcess.CommandLine.Replace('\', '/').IndexOf($normalizedProject, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw '这个工程仍在 UE 中打开。请保存并关闭工程，再运行安装器。'
        }
    }

    if (!(Test-Path -LiteralPath (Join-Path $pluginSource 'MaterialChineseHelper.uplugin')) -or
        !(Test-Path -LiteralPath (Join-Path $pluginSource 'Binaries\Win64\UnrealEditor-MaterialChineseHelper.dll'))) {
        throw '安装包不完整，请先完整解压，或使用包含 Binaries 的完整安装包。'
    }
    if (Test-Path -LiteralPath $pluginTarget) {
        $existingDescriptor = Join-Path $pluginTarget 'MaterialChineseHelper.uplugin'
        if (!(Test-Path -LiteralPath $existingDescriptor)) {
            throw '目标已有同名文件夹，但不是材质中文助手。为避免覆盖，请先检查该目录。'
        }
        $oldDescriptor = Get-Content -LiteralPath $existingDescriptor -Raw -Encoding UTF8 | ConvertFrom-Json
        if (!($oldDescriptor.Modules | Where-Object { $_.Name -eq 'MaterialChineseHelper' })) {
            throw '已有插件的模块不匹配，安装器已停止。'
        }
        $backupRoot = Join-Path $projectRoot 'Saved\MaterialChineseHelperBackups'
        $backupPath = Join-Path $backupRoot ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '\MaterialChineseHelper')
        $null = New-Item -ItemType Directory -Path $backupPath -Force
        Get-ChildItem -LiteralPath $pluginTarget -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $backupPath -Recurse
        }
        Write-Host "旧版已备份：$backupPath"
    } else {
        $null = New-Item -ItemType Directory -Path $pluginTarget
    }
    Get-ChildItem -LiteralPath $pluginSource -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $pluginTarget -Recurse -Force
    }
    Write-Host "安装完成：$pluginTarget" -ForegroundColor Green
    Write-Host '重新打开工程，在 工具 / Tools → 材质中文助手 中查看中文词典和设置。'
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
