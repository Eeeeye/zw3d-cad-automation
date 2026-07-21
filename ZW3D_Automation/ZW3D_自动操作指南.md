# ZW3D 自动操作指南

## 🎯 自动执行命令的方法

我已经为你创建了多种自动操作 ZW3D 的方式，无需手动输入命令！

## 📁 自动操作脚本

### 1. ZW3D-QuickLaunch.bat (推荐)
**位置**: `C:\Users\Ey\.openclaw\workspace\ZW3D-QuickLaunch.bat`

**使用方法**:
1. 双击运行 `ZW3D-QuickLaunch.bat`
2. 选择要执行的命令（输入 1-6）
3. 脚本会自动将命令发送到 ZW3D

**功能**:
- 自动检测 ZW3D 是否运行
- 自动激活 ZW3D 窗口
- 自动发送命令到命令行
- 无需手动输入

### 2. Direct-ZW3D-Execute.ps1
**位置**: `C:\Users\Ey\.openclaw\workspace\Direct-ZW3D-Execute.ps1`

**使用方法**:
```powershell
# 执行 Hello 命令
.\Direct-ZW3D-Execute.ps1 -Action "Hello"

# 创建复杂形状
.\Direct-ZW3D-Execute.ps1 -Action "ComplexShape"

# 创建齿轮
.\Direct-ZW3D-Execute.ps1 -Action "Gear"

# 其他选项: Assembly, Annotate, Note
```

### 3. ZW3D-SimpleAuto.ps1
**位置**: `C:\Users\Ey\.openclaw\workspace\ZW3D-SimpleAuto.ps1`

**使用方法**:
```powershell
.\ZW3D-SimpleAuto.ps1
# 然后按提示输入数字选择命令
```

## 🚀 快速使用步骤

### 方法 1: 使用批处理文件（最简单）

1. **打开文件夹**:
   ```
   C:\Users\Ey\.openclaw\workspace\
   ```

2. **双击运行**: `ZW3D-QuickLaunch.bat`

3. **选择命令**:
   ```
   [1] Hello Test
   [2] Create Complex Shape
   [3] Create Gear
   [4] Create Assembly
   [5] Auto Annotate
   [6] Add Note
   ```

4. **输入数字**（如：2）然后按 Enter

5. **查看 ZW3D** - 命令已自动执行！

### 方法 2: 使用 PowerShell

```powershell
# 打开 PowerShell
# 切换到工作目录
cd C:\Users\Ey\.openclaw\workspace

# 执行命令
.\Direct-ZW3D-Execute.ps1 -Action "ComplexShape"
```

## 📋 可用命令列表

| 编号 | 命令 | 功能 |
|------|------|------|
| 1 | ~HelloZW3D | 基础测试 |
| 2 | ~ZW3DCreateComplexShape | 创建组合形状 |
| 3 | ~ZW3DCreateGearShape | 创建齿轮 |
| 4 | ~ZW3DCreateAssembly | 创建装配体 |
| 5 | ~ZW3DAutoAnnotatePart | 自动标注分析 |
| 6 | ~ZW3DAddNote | 添加注释 |

## 🔧 自动化原理

脚本通过以下步骤自动操作 ZW3D：

1. **检测 ZW3D**: 检查 ZW3D 是否运行
2. **激活窗口**: 使用 `AppActivate` 激活 ZW3D
3. **激活命令行**: 发送 `F2` 键激活命令行
4. **输入命令**: 自动输入命令文本
5. **执行命令**: 发送 `Enter` 键执行

```powershell
# 伪代码
$wshell = New-Object -ComObject wscript.shell
$wshell.AppActivate($zw3d.Id)      # 激活窗口
$wshell.SendKeys("{F2}")            # 激活命令行
$wshell.SendKeys("~Command")        # 输入命令
$wshell.SendKeys("{ENTER}")         # 执行
```

## 💡 使用建议

### 工作流 1: 快速创建几何
1. 双击 `ZW3D-QuickLaunch.bat`
2. 选择 2（创建复杂形状）
3. 在 ZW3D 中查看结果

### 工作流 2: 自动标注
1. 先在 ZW3D 中创建零件
2. 运行批处理文件
3. 选择 5（自动标注）
4. 查看分析报告

### 工作流 3: 批量操作
可以修改脚本，连续执行多个命令：

```powershell
# 示例：连续执行多个命令
.\Direct-ZW3D-Execute.ps1 -Action "ComplexShape"
Start-Sleep -Seconds 3
.\Direct-ZW3D-Execute.ps1 -Action "Annotate"
```

## 🎓 进阶用法

### 创建桌面快捷方式

1. 右键点击 `ZW3D-QuickLaunch.bat`
2. 选择 "发送到 > 桌面快捷方式"
3. 双击桌面图标即可快速启动

### 创建特定命令的快捷方式

创建 `CreateGear.bat`:
```batch
@echo off
powershell -ExecutionPolicy Bypass -File "C:\Users\Ey\.openclaw\workspace\Direct-ZW3D-Execute.ps1" -Action "Gear"
```

## 📊 文件清单

| 文件 | 用途 |
|------|------|
| ZW3D-QuickLaunch.bat | 交互式命令选择 |
| Direct-ZW3D-Execute.ps1 | 直接执行指定命令 |
| ZW3D-SimpleAuto.ps1 | 简单交互菜单 |
| Auto-Execute-ZW3DCommands.ps1 | 高级自动执行 |

## ✅ 验证自动操作

测试自动操作是否工作：

1. 确保 ZW3D 正在运行
2. 双击 `ZW3D-QuickLaunch.bat`
3. 输入 `1` 选择 Hello Test
4. 查看 ZW3D 消息窗口
5. 如果看到 "Hello ZW3D!"，说明自动操作成功！

## 🎯 总结

现在你可以：
- ✅ 双击批处理文件自动执行命令
- ✅ 无需手动输入命令
- ✅ 一键创建几何体
- ✅ 一键执行标注分析
- ✅ 完全自动化 ZW3D 操作

---

**创建时间**: 2026-03-16  
**创建者**: 小爪 🦊  
**状态**: ✅ 自动操作功能已完成
