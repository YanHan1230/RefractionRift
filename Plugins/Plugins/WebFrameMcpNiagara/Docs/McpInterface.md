# Niagara MCP 模块

## 简要说明
Niagara 模块用于通过 MCP 操作 NiagaraSystem 和 NiagaraEmitter 资产。当前重点覆盖资产创建、结构读取、系统发射器、用户参数、Renderer、基础 Stack 模块插入、图节点、Pin 默认值、节点连线、编译请求，以及常用特效预设生成。

这个模块适合让 AI 先搭出可运行、可继续调参的 Niagara 资产骨架，再由后续 MCP 调用或人工在 Niagara 编辑器中细化材质、动态输入、Scratch Pad 和高级 Stack 参数。基础图编辑接口支持反复修正节点位置、错误连线和多余节点。

## MCP 接口
- `niagara.system.list`：查询 NiagaraSystem 资产。支持 `path` / `root` / `limit` 一类路径限制参数，避免一次扫描过多资产。
- `niagara.emitter.list`：查询 NiagaraEmitter 资产。
- `niagara.system.create`：创建 NiagaraSystem 资产。
- `niagara.emitter.create`：创建 NiagaraEmitter 资产。
- `niagara.describe`：读取 NiagaraSystem 或 NiagaraEmitter 的结构信息，包括用户参数、发射器、图节点和 Renderer 列表。
- `niagara.system.add_emitter`：把 NiagaraEmitter 资产添加到 NiagaraSystem。
- `niagara.system.remove_emitter`：从 NiagaraSystem 移除发射器句柄。
- `niagara.emitter.set_enabled`：启用或禁用系统中的发射器句柄。
- `niagara.emitter.set_property`：设置发射器数据属性。常用属性包括 `localSpace` / `bLocalSpace`、`simTarget`，也支持通过 UE 属性名导入简单结构体属性。
- `niagara.user_parameter.add`：新增 NiagaraSystem 用户参数。
- `niagara.user_parameter.remove`：删除 NiagaraSystem 用户参数。
- `niagara.user_parameter.set`：设置 NiagaraSystem 用户参数值。
- `niagara.renderer.list`：查询目标发射器的 Renderer 列表。
- `niagara.renderer.add`：向目标发射器添加 Renderer。支持 `sprite`、`mesh`、`ribbon`、`light`。
- `niagara.renderer.remove`：从目标发射器删除指定 Renderer。
- `niagara.renderer.set_property`：设置 Renderer 属性。常用快捷属性包括 `enabled`、`material`、`mesh`，其他属性可使用 UE 属性名和文本导入值。
- `niagara.module.add_if_missing`：按 Usage Stack 添加缺失的 Niagara 模块脚本。
- `niagara.graph.node.add`：添加 Niagara 图节点。支持 function、input、output、op、assignment 等基础节点别名。
- `niagara.graph.node.set_property`：设置 Niagara 图节点属性。
- `niagara.graph.node.move`：移动 Niagara 图节点的编辑器坐标。
- `niagara.graph.node.delete`：删除 Niagara 图节点，并断开该节点所有连线。
- `niagara.graph.pin.set_default`：设置 Niagara 图 Pin 默认值。
- `niagara.graph.node.connect`：连接两个 Niagara 图 Pin。
- `niagara.graph.pin.disconnect`：断开 Niagara 图节点某个 Pin 的连接，或指定两端 Pin 断开单条连接。
- `niagara.graph.node.disconnect`：`niagara.graph.pin.disconnect` 的别名；不传 `pin` 时断开目标节点所有连线。
- `niagara.compile`：请求编译 NiagaraSystem。
- `niagara.preset.create`：创建实用 Niagara 预设系统。当前支持 `fish_school`、`jet_exhaust`、`sonic_boom`。

## 常用参数说明
- `asset`：目标 Niagara 资产路径，例如 `/Game/Niagara/NS_Test.NS_Test`。未带对象名时模块会尝试补齐。
- `path`：创建资产或查询资产时使用的 UE 内容路径，例如 `/Game/Niagara`。
- `name`：新资产名、参数名、Renderer 名等，具体含义取决于接口。
- `emitter`：系统内发射器名称或 id。访问 NiagaraSystem 内部 emitter 图、属性、Renderer 时通常必须提供。
- `preset`：预设类型。`fish_school` 生成鱼群占位效果，`jet_exhaust` 生成喷气机尾焰骨架，`sonic_boom` 生成音爆/蒸汽锥骨架。
- `usage`：目标脚本用途，例如 `systemUpdate`、`emitterUpdate`、`particleSpawn`、`particleUpdate`。
- `module`：要插入的 Niagara 模块脚本路径，例如 `/Niagara/Modules/Update/Forces/CurlNoiseForce.CurlNoiseForce`。
- `type`：用户参数类型或 Renderer 类型。参数常用 `float`、`int`、`bool`、`vec2`、`vec3`、`vec4`、`color`、`position`；Renderer 常用 `sprite`、`mesh`、`ribbon`、`light`。
- `value`：要写入的值。向量和颜色使用数组，例如 `[1, 2, 3]` 或 `[1, 0.5, 0.2, 1]`。
- `material`：Renderer 使用的材质路径。
- `mesh`：Mesh Renderer 使用的 StaticMesh 路径。
- `renderer` / `index`：定位目标 Renderer。可以用 Renderer 名称、类名、类路径，或列表中的索引。
- `node`：Niagara 图节点 GUID、标题或对象名。
- `pin`：Niagara 图 Pin 的名称或 GUID。
- `fromNode` / `fromPin` / `toNode` / `toPin`：连接 Niagara 图节点时使用的两端节点和 Pin。
- `otherNode` / `otherPin`：断开单条 Niagara 图连接时使用的另一端节点和 Pin。
- `x` / `y`：图节点编辑器坐标。
- `save`：是否保存资产包。写操作默认会标记 dirty，部分预设创建默认保存。

## 预设说明
- `fish_school`：创建鱼群自由游动的基础系统。当前使用 `/Engine/BasicShapes/Sphere` 作为鱼模型占位，包含球形生成、锥形速度、Curl Noise 游动、速度求解，以及 `FishSpawnRate`、`SwimRadius`、`SwimSpeed`、`WanderStrength`、`LifetimeMin`、`LifetimeMax` 等用户参数。
- `jet_exhaust`：创建喷气机尾焰基础系统。包含火焰核心 sprite 层和热扰动 ribbon 层，暴露 `ExhaustSpawnRate`、`ExhaustLength`、`ExhaustSpread`、`HeatIntensity`、`SmokeFadeTime` 等用户参数。
- `sonic_boom`：创建战斗机音爆/蒸汽锥基础系统。包含蒸汽锥 sprite 层和冲击环 ribbon 层，暴露 `ConeRadius`、`ConeLength`、`VaporDensity`、`ShockStrength`、`DissolveTime` 等用户参数。

## 当前覆盖情况
- 已覆盖 NiagaraSystem / NiagaraEmitter 资产创建、查询和描述。
- 已覆盖系统内 emitter 句柄增删、启用禁用和部分 emitter data 属性编辑。
- 已覆盖用户参数增删改，支持常见数值、向量、颜色类型。
- 已覆盖 Renderer 查询、添加和常见属性设置，支持 sprite、mesh、ribbon、light。
- 已覆盖 Renderer 删除。
- 已覆盖基础 Niagara 图节点创建、节点属性、节点移动、节点删除、Pin 默认值、Pin 连线和断线。
- 已覆盖通过公开 NiagaraScriptSource API 向指定 Usage Stack 添加模块脚本。
- 已覆盖创建常见特效预设骨架，便于快速生成可继续编辑的 NiagaraSystem。
- 所有写操作应记录到 MCP 控制台、小时日志文件和插件“操作记录”列表。

## 待增强功能
- 更完整的 Stack 参数绑定和 Rapid Iteration 参数精确写入。
- Scratch Pad 脚本自动生成与复杂动态输入树创建。
- 更完整的 Niagara 数据接口、事件处理器、Simulation Stage 管理。
- 自动创建专用 Niagara 材质、动态材质实例和更细的 renderer binding 配置。
- 对 GPU Niagara 高级模块、碰撞、距离场、体积渲染等复杂配置的高层封装。

## 注意事项
- Niagara 的最终正确性以 UE 编译结果和 Niagara 编辑器预览为准。
- 当前预设偏“可用骨架”，不是最终美术资源。鱼模型、尾焰材质、蒸汽锥材质建议后续替换成项目专用资产。
- 编辑 NiagaraSystem 内部 emitter 时，通常需要提供 `emitter`，因为一个系统可能包含多个同名风格接近的发射器。
- `niagara.module.add_if_missing` 使用公开 NiagaraEditor API，比直接操作私有 Stack ViewModel 稳定，但对复杂模块输入的自动配置能力有限。
- 如果启用了路径权限，Niagara 模块只能访问或修改被允许的 `/Game/...` 或 `/All/Game/...` 路径。
