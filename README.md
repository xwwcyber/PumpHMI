# PumpHMI · 工业水泵监控上位机

基于 Qt 6 的工业水泵监控 HMI（人机界面），通过 Modbus TCP 与下位机（PLC 或模拟器）通信，实现实时数据采集、趋势曲线、超限报警和远程启停。

## 功能

- **Modbus TCP 通信** —— 每秒轮询读取压力、温度、水泵状态
- **实时压力曲线** —— 基于 QtCharts 的滑动窗口趋势图（最近 60 秒）
- **超限报警** —— 压力超过阈值时弹窗提示并将记录写入 SQLite
- **远程启停** —— 一键写寄存器控制水泵启停
- **可独立部署** —— 通过 `windeployqt` 打包，脱离 Qt 环境运行

## 技术栈

Qt 6.11 · MinGW 64-bit · QtWidgets · QtSerialBus · QtCharts · QtSql

## 构建

需安装 Qt 6（勾选 **Qt Charts**、**Qt Serial Bus** 模块）。用 Qt Creator 打开 `CMakeLists.txt` 直接构建，或命令行：

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

## 运行

HMI 是上位机，需要一个 Modbus 下位机才能通信。仓库内提供了一个基于 `pymodbus` 的智能模拟器，会模拟泵启停时的压力升降物理过程：

```bash
pip install pymodbus
python mock_slave.py        # 以管理员身份运行，502 端口需要权限
```

模拟器寄存器约定：

| 地址 | 含义     | 编码           |
| ---- | -------- | -------------- |
| 0    | 压力     | 实际值 × 100   |
| 1    | 温度     | 实际值 × 10    |
| 2    | 水泵状态 | 0=停止，1=运行 |

启动模拟器后运行 PumpHMI，点击"启动水泵"即可看到实时曲线。

## 项目结构

```
PumpHMI/
├── CMakeLists.txt     CMake 工程文件
├── main.cpp           程序入口
├── mainwindow.h       主窗口声明
├── mainwindow.cpp     主窗口实现（业务逻辑）
└── mock_slave.py      Python 智能 Modbus 模拟器
```
