# 编辑器世界 MCP 模块

## 简要说明
编辑器世界模块用于操作当前已加载编辑器世界中的 Actor，支持对象列表、对象描述、Actor 放置、Transform 修改、实例属性修改、Bounds 查询、组件几何查询和 PostProcessVolume 配置。

## MCP 接口
- `world.actor.list`：查询当前编辑器世界中已加载的 Actor。
- `world.actor.describe`：读取 Actor 及其组件信息。
- `world.actor.spawn`：在当前编辑器世界中放置 Actor。
- `world.actor.set_transform`：设置 Actor 的位置、旋转和缩放。
- `world.actor.set_property`：设置 Actor 或组件实例属性。
- `world.actor.get_bounds`：获取 Actor Bounds。
- `world.actor.mesh_geometry`：读取 StaticMeshComponent 的世界空间几何信息。
- `world.actor.mesh_geometry.describe`：几何查询接口别名。
- `world.postprocess.spawn_volume`：放置 PostProcessVolume。
- `world.postprocess.add_material`：给 PostProcessVolume 添加或更新后处理材质。

## 常用参数说明
- `actorRef` / `actorGuid` / `label` / `name`：定位世界中的 Actor。
- `componentRef`：定位 Actor 里的组件。
- `classPath`：放置 Actor 时使用的类路径。
- `location` / `rotation` / `scale`：Actor Transform。
- `propertyName` / `value`：要修改的实例属性和值。
- `unbound`：PostProcessVolume 是否无边界影响全局。

## 注意事项
- 当前只处理已加载编辑器世界对象，不处理未加载的 World Partition Actor。
- Actor 名称和 Label 可能重复，能提供路径或 GUID 时应优先使用。
- 实例属性可能会被蓝图 Construction Script 覆盖。
