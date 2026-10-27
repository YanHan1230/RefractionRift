# 材质 MCP 模块

## 简要说明
材质模块用于操作 Material 和 MaterialInstance 资产，支持材质创建、材质属性设置、表达式节点创建、表达式属性修改、节点连线、输出属性连接、节点复制、节点自动整理、节点移动、节点删除、断开输入、断开输出属性、材质实例创建、实例参数设置和材质编译验证。

## MCP 接口
- `material.list`：按路径、数量、偏移和递归选项查询材质资产。
- `material.create`：创建材质资产。
- `material.describe`：读取材质属性和表达式图结构；如果目标是材质实例，则读取父材质、参数覆盖和可见参数值。
- `material.set_settings`：设置材质域、混合模式、着色模型、透明、后处理、自定义深度/模板等材质级属性。
- `material.instance.create`：创建 `MaterialInstanceConstant` 资产，并指定父材质或父材质实例。
- `material.instance.set_parameter`：设置材质实例的 `scalar`、`vector/color`、`texture` 参数，或替换 `parent`。
- `material.node.add`：添加材质表达式节点。
- `material.node.set_property`：设置材质表达式节点属性。
- `material.node.connect`：连接两个材质表达式节点。支持连接到 Custom 节点 `Inputs` 数组里的命名输入。
- `material.connect_property`：把表达式输出连接到材质输出属性。
- `material.node.duplicate`：复制指定材质表达式节点，可指定复制后节点坐标。
- `material.node.layout`：调用 UE 材质编辑器布局工具自动整理表达式节点。
- `material.node.move`：移动指定材质表达式节点的编辑器坐标。
- `material.node.delete`：删除指定材质表达式节点，并清理到该节点的连接。
- `material.node.disconnect`：断开指定表达式输入；不指定 `input` 时断开该表达式所有已连接输入。
- `material.disconnect_property`：断开材质输出属性，例如 BaseColor、Emissive、Opacity 等。
- `material.compile`：显式触发目标材质重新编译，可选保存，并返回最近日志诊断结果。
- `material.diagnostics`：从当前项目日志中读取某个材质最近的编译错误或材质编辑器错误。

## 常用参数说明
- `asset`：目标材质资产路径。
- `materialDomain`：材质域，例如 Surface、PostProcess、UserInterface。
- `blendMode`：混合模式，例如 Opaque、Masked、Translucent。
- `nodeType`：材质表达式节点类型。
- `expression`：材质表达式节点名称。
- `fromExpression` / `fromOutput`：连线源表达式和输出名称。
- `toExpression` / `toInput`：连线目标表达式和输入名称。
- `input`：断开表达式输入时使用的目标输入名称。
- `property`：材质输出属性名称或表达式属性名称，取决于接口。
- `x` / `y`：表达式节点在材质图中的编辑器坐标。
- `parent` / `parentMaterial`：创建材质实例时使用的父材质接口路径。
- `parameter`：材质实例参数名。
- `type`：材质实例参数类型，支持 `scalar`、`vector`、`color`、`texture`、`parent`。
- `value`：材质实例参数值。标量使用数字；向量/颜色使用 `[r,g,b,a]` 或 `{r,g,b,a}`；贴图和父材质使用资产路径。
- `lookbackSeconds`：诊断日志回看时间，默认 600 秒；设为 0 表示不按时间过滤。
- `maxLines`：诊断返回的最大日志行数，默认 80。

## 注意事项
- 不同材质域允许的输出属性不同，例如后处理材质和 UI 材质不能完全复用 Surface 材质的输出。
- MCP 会尽量检查连线，但最终是否合法仍以 UE 材质编译结果为准。
- 修改材质后建议调用 `material.compile`，再查看返回的 `diagnostics.hasErrors` 和 `diagnostics.lines`。
- 复杂材质图建议显式指定节点名称和坐标，方便后续继续编辑。
- Custom 节点输入需要先通过 `material.node.add` 的 `properties.Inputs` 创建，例如 `((InputName="WorldPos"),(InputName="Interval"))`，然后才能用 `material.node.connect` 按名称连接。
- `material.node.layout` 会重排整个材质图，适合生成完成后整理视图，不适合局部微调坐标时调用。
- `material.node.delete` 会清理指向该表达式的连接；如果只是想重连图，优先使用 `material.node.disconnect` 或 `material.disconnect_property`。
