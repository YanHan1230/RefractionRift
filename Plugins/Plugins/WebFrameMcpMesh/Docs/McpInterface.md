# 模型 MCP 模块

## 简要说明
模型模块用于读取和修改静态模型、骨骼模型以及静态模型组件相关信息，当前重点支持模型描述、几何采样、材质槽和 Socket 操作。

## MCP 接口
- `mesh.static.list`：查询 StaticMesh 资产。
- `mesh.skeletal.list`：查询 SkeletalMesh 资产。
- `mesh.describe`：读取模型元数据、Bounds、材质槽和 Socket。
- `mesh.geometry.describe`：读取 StaticMesh 顶点、三角面和相对球心的距离范围。
- `mesh.material_slot.set`：设置模型材质槽。
- `mesh.socket.add`：给 StaticMesh 添加 Socket，或给 SkeletalMesh 对应的 Skeleton 添加 Socket。
- `mesh.socket.update`：更新 Socket 的相对位置、旋转和缩放。
- `mesh.socket.remove`：删除 Socket。

## 常用参数说明
- `assetPath`：目标模型资产路径。
- `lodIndex`：几何采样使用的 LOD。
- `vertexLimit` / `triangleLimit`：限制返回顶点和三角面数量。
- `socketName`：Socket 名称。
- `boneName`：骨骼 Socket 绑定的骨骼名称。
- `relativeLocation` / `relativeRotation` / `relativeScale`：Socket 的相对变换。

## 注意事项
- SkeletalMesh 的 Socket 会写到 Skeleton 资产上，这样同一 Skeleton 下的多个 SkeletalMesh 都可能受到影响。
- 几何采样返回数据可能很大，应设置合理的数量限制。
- StaticMeshComponent 的实例几何查询归属于编辑器世界模块。
