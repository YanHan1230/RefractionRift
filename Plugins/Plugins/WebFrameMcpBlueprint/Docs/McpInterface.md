# 普通蓝图 MCP 模块

## 简要说明
普通蓝图模块用于操作常规 Blueprint 资产，支持资产查询、创建、结构读取、变量增删改、函数、宏、自定义事件、注释节点、节点放置、Pin 默认值、节点连线、节点移动、节点删除、断开连线和编译。

## MCP 接口
- `blueprint.list`：按路径、数量、偏移和递归选项查询蓝图资产。
- `blueprint.create`：创建蓝图资产，可指定父类，支持自定义 C++ 类的子类蓝图。
- `blueprint.can_create_child`：检查指定父类是否能创建子类蓝图。
- `blueprint.describe`：读取蓝图结构，包括图、函数、宏、变量、节点和 Pin 信息。
- `blueprint.variables`：查询蓝图变量列表。
- `blueprint.variable.add`：新增蓝图变量。
- `blueprint.variable.rename`：重命名蓝图变量，并通过 UE 工具函数更新已有变量引用。
- `blueprint.variable.remove`：删除蓝图变量，并移除对应变量节点。
- `blueprint.set_variable_default`：设置蓝图变量默认值。
- `blueprint.set_class_default`：设置蓝图类默认属性。
- `blueprint.set_component_default`：设置蓝图组件模板默认属性。
- `blueprint.set_pin_default`：设置节点 Pin 默认值。
- `blueprint.graph.add_function`：新增函数图。
- `blueprint.graph.add_macro`：新增宏图。
- `blueprint.node.call_function`：添加函数调用节点。
- `blueprint.node.custom_event`：添加自定义事件节点。
- `blueprint.node.comment`：添加注释节点。
- `blueprint.node.variable_get`：添加变量 Get 节点。
- `blueprint.node.variable_set`：添加变量 Set 节点。
- `blueprint.node.connect`：连接两个蓝图节点的 Pin。
- `blueprint.node.move`：移动指定蓝图图节点的位置。
- `blueprint.node.delete`：删除指定蓝图图节点，并断开该节点所有连线。
- `blueprint.node.disconnect`：断开指定节点、指定 Pin，或指定两端 Pin 之间的连线。
- `blueprint.compile`：编译蓝图资产。

## 常用参数说明
- `asset`：目标蓝图资产路径。
- `path` / `name`：创建蓝图时使用的目录和资产名。
- `parentClass`：父类路径或类名。
- `graphName` / `graphPath`：目标图名称或图路径。
- `nodeId`：节点 GUID。
- `pinId` / `pinName`：Pin 的 GUID 或名称。
- `fromNodeId` / `fromPinId` / `fromPinName`：连线源节点和源 Pin。
- `toNodeId` / `toPinId` / `toPinName`：连线目标节点和目标 Pin。
- `otherNodeId` / `otherPinId` / `otherPinName`：断开单条连接时的另一端节点和 Pin。
- `x` / `y`：节点在图中的编辑器坐标。
- `defaultValue`：变量、属性或 Pin 的默认值文本。
- `compile` / `save`：写操作后是否立即编译或保存。
- `oldName` / `newName`：变量重命名时使用的旧变量名和新变量名。

## 注意事项
- 大项目中应使用 `path` 和 `limit` 限制 `blueprint.list`，避免一次扫描过多资产。
- 图和节点修改后建议调用 `blueprint.compile`，让编辑器状态及时刷新。
- 组件默认属性修改的是蓝图类模板，不是已经放在世界里的 Actor 实例。
- `blueprint.node.delete` 会直接移除节点，建议先用 `blueprint.describe` 获取节点 GUID 并确认目标。
- `blueprint.variable.rename` 会尝试更新变量引用；`blueprint.variable.remove` 会移除变量节点。对已经大量使用的变量，建议先读取蓝图结构后再操作。
