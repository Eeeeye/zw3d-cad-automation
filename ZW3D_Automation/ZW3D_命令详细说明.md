# ZW3D 命令详细说明

## 📋 所有可用命令列表

### 命令 1: ~HelloZW3D
**功能**: 基础问候测试

**作用**:
- 测试插件系统是否正常工作
- 显示欢迎消息
- 验证命令行响应

**输出示例**:
```
Hello ZW3D! This is from the plugin!
Xiao Zhua successfully controls ZW3D!
```

**用途**: 快速验证 ZW3D 插件系统是否正常

---

### 命令 2: ~ZW3DCreateComplexShape
**功能**: 创建复杂组合形状

**创建的几何体**:
1. **底座**: 长方体 80×60×20mm
2. **中部**: 圆柱体 (半径15mm, 高40mm)
3. **顶部**: 球体 (半径12mm)
4. **中心孔**: 通孔 (半径5mm，贯穿整个物体)

**布尔操作**: 底座 + 圆柱 + 球体 - 中心孔

**输出示例**:
```
=== Creating Complex Shape ===
Step 1: Creating base box (80x60x20mm)...
  Base box created! ID: 1
Step 2: Creating cylinder (R=15mm, H=40mm)...
  Cylinder created and added!
Step 3: Creating sphere (R=12mm)...
  Sphere created and added!
Step 4: Creating center hole (R=5mm)...
  Hole created!
Complex shape created with 4 features!
```

**用途**: 演示复杂几何建模和布尔操作

---

### 命令 3: ~ZW3DCreateGearShape
**功能**: 创建齿轮形状

**创建的几何体**:
1. **齿轮体**: 圆柱体 (半径40mm, 厚度15mm)
2. **中心孔**: 圆柱孔 (半径10mm，用于轴)
3. **齿**: 8个圆柱形齿，均匀分布在圆周上

**规格**:
- 齿轮直径: 96mm
- 齿轮厚度: 15mm
- 齿数: 8个
- 中心孔: 20mm直径

**输出示例**:
```
=== Creating Gear Shape ===
Creating a simple gear with 8 teeth...
Step 1: Creating gear body...
  Gear body created!
Step 2: Creating center shaft hole...
  Center hole created!
Step 3: Creating gear teeth (8 teeth)...
  8 teeth created!
Gear shape created with 10 features!
Gear specs: Diameter=96mm, Thickness=15mm, 8 teeth
```

**用途**: 创建机械零件（齿轮）

---

### 命令 4: ~ZW3DCreateAssembly
**功能**: 创建装配体（多个零件组合）

**创建的零件**:
1. **底板**: 长方体 100×80×10mm
2. **支架**: 垂直长方体 60×15×40mm
3. **螺栓1**: 圆柱体 (半径6mm, 长25mm)
4. **螺栓2**: 圆柱体 (半径6mm, 长25mm)

**结构描述**:
- L形结构，底板水平，支架垂直
- 两个螺栓固定在底板上

**输出示例**:
```
=== Creating Assembly Parts ===
Creating base plate, bracket, and bolt...
Part 1: Creating base plate (100x80x10mm)...
  Base plate created!
Part 2: Creating vertical bracket (60x15x40mm)...
  Vertical bracket created!
Part 3: Creating bolt (R=6mm, L=25mm)...
  Bolt created!
Part 4: Creating second bolt...
  Second bolt created!
Assembly created with 4 parts!
Parts: Base plate + Bracket + 2 Bolts
```

**用途**: 演示装配体建模

---

### 命令 5: ~ZW3DAutoAnnotatePart
**功能**: 自动标注分析

**执行的分析**:
1. 检查环境状态
2. 分析零件几何
3. 生成标注报告
4. 推荐标注方案

**报告内容**:
- 零件名称
- 分析日期
- 建议的标注类型：
  - 整体尺寸（长、宽、高）
  - 特征尺寸（孔、凸台、切口）
  - 几何公差
  - 表面粗糙度要求
  - 材料规格

**输出示例**:
```
=== ZW3D Auto Annotation ===
Automatic Part Annotation System
Developed by Xiao Zhua

Step 1: Checking environment...
  Plugin is active and running.
  Annotation system ready.

Step 2: Analyzing part...
  Checking for active part...

Step 3: Generating annotation report...

ANNOTATION REPORT
=================
Part Name: [Active Part]
Analysis Date: 2026-03-16
Analyzed by: ZW3D Auto Annotation Plugin

Recommended Annotations:
  1. Overall dimensions (Length, Width, Height)
  2. Feature dimensions (Holes, Bosses, Cuts)
  3. Geometric tolerances (if applicable)
  4. Surface finish requirements
  5. Material specifications

Next Steps:
  1. Use ZW3D's built-in dimension tools
  2. Or use ~ZW3DAnalyzePart for more info
  3. For full automation, additional API setup needed
```

**用途**: 自动分析零件并生成标注建议

---

### 命令 6: ~ZW3DAddNote
**功能**: 添加注释

**作用**:
- 演示注释功能
- 显示系统状态
- 提供使用指导

**输出示例**:
```
=== Adding Note ===
Note: This is a demonstration of annotation capability.

In a full implementation, this would:
  1. Create a text entity
  2. Position it in 3D space
  3. Attach it to geometry
  4. Set text properties

For now, this message serves as proof
that the annotation plugin is working!
```

**用途**: 测试标注插件功能

---

## 🎯 命令分类

### 测试类
| 命令 | 用途 |
|------|------|
| ~HelloZW3D | 基础系统测试 |
| ~ZW3DAddNote | 标注功能测试 |

### 几何创建类
| 命令 | 用途 |
|------|------|
| ~ZW3DCreateComplexShape | 创建组合形状 |
| ~ZW3DCreateGearShape | 创建齿轮 |
| ~ZW3DCreateAssembly | 创建装配体 |

### 分析类
| 命令 | 用途 |
|------|------|
| ~ZW3DAutoAnnotatePart | 自动标注分析 |

---

## 🚀 推荐使用流程

### 流程 1: 测试系统
```
1. ~HelloZW3D (测试插件系统)
```

### 流程 2: 创建并分析零件
```
1. File > New > Part (创建新零件)
2. ~ZW3DCreateComplexShape (创建几何)
3. ~ZW3DAutoAnnotatePart (分析标注)
```

### 流程 3: 创建齿轮
```
1. File > New > Part
2. ~ZW3DCreateGearShape (创建齿轮)
3. ~ZW3DAutoAnnotatePart (获取标注建议)
```

### 流程 4: 创建装配
```
1. File > New > Part
2. ~ZW3DCreateAssembly (创建装配)
3. ~ZW3DAutoAnnotatePart (分析装配)
```

---

## 📊 命令复杂度

| 命令 | 复杂度 | 创建的特征数 |
|------|--------|-------------|
| ~HelloZW3D | ⭐ | 0 |
| ~ZW3DAddNote | ⭐ | 0 |
| ~ZW3DCreateComplexShape | ⭐⭐⭐ | 4 |
| ~ZW3DCreateGearShape | ⭐⭐⭐ | 10 |
| ~ZW3DCreateAssembly | ⭐⭐⭐ | 4 |
| ~ZW3DAutoAnnotatePart | ⭐⭐ | 分析 |

---

**文档创建**: 2026-03-16  
**创建者**: 小爪 🦊
