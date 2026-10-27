# Refraction Rift / 折光断层

## 项目定位

UE 5.5、C++ 与蓝图混合的技术美术展示型独立游戏。
核心展示：PBR、材质函数、Custom HLSL、溶解、扫描、Fresnel、Niagara、Lumen、性能分析。

## Content 目录

- `00_Core`：项目级数据、输入、接口
- `01_Levels`：关卡与测试房间
- `02_Materials`：材质主材质、实例、函数、参数集合
- `03_Textures`：噪声、遮罩和表面贴图
- `04_VFX`：Niagara 特效及其材质
- `05_Blueprints`：交互物体、组件、系统和 UI
- `06_Environment`：环境模型、材质、植被和 PCG
- `07_Characters`：玩家与动画
- `08_Audio`：音乐和音效
- `09_Cinematics`：演出内容
- `10_Debug`：材质测试与性能验证

## 命名约定

- `M_` 主材质；`MI_` 材质实例；`MF_` 材质函数；`MPC_` 材质参数集合
- `BP_` 蓝图；`BPI_` 蓝图接口；`AC_` Actor Component；`WBP_` 界面
- `NS_` Niagara 系统；`NE_` Niagara 发射器；`LV_` 关卡
- 技术美术实验资源统一放入 `10_Debug`，确认可复用后再移动到正式目录

## 第一阶段

1. 在 `01_Levels/TestRoom` 建立测试房间。
2. 在 `02_Materials/Master` 建立溶解主材质 `M_Dissolve_Master`。
3. 在 `02_Materials/ParameterCollections` 建立全局参数集合 `MPC_Global`。
4. 在 `05_Blueprints/Actors` 建立第一个可交互物体。
5. 在 `10_Debug/MaterialTests` 保留材质球、方块和参数测试用例。
