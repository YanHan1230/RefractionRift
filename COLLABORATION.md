# Refraction Rift 协作规则

## 提交范围

提交 `Config`、`Content`、`Source`、`Plugins`、`.uproject` 和项目文档。不要提交 `Binaries`、`DerivedDataCache`、`Intermediate`、`Saved`、`.vs` 或 `.sln`。

## 日常流程

1. 开始工作前：`git pull`，再执行 `git lfs pull`。
2. 新功能使用分支：`feature/功能名`。
3. 修改 UE 二进制资源前先锁定：`git lfs lock Content/路径/资源.uasset`。
4. 完成后提交并推送分支，再创建 GitHub Pull Request。
5. 合并后解除锁定：`git lfs unlock Content/路径/资源.uasset`。

## 分工建议

- 美术与材质：`Content/RefractionRift/02_Materials`、`03_Textures`、`06_Environment`
- 技术与交互：`Source`、`Content/RefractionRift/00_Core`、`05_Blueprints`、`10_Debug`
- 同一个 `.uasset` 或 `.umap` 同时只能由一个人编辑。
