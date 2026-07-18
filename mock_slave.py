"""
PumpHMI 智能 Modbus Slave 模拟器
模拟一个有"物理行为"的下位机：
  - 寄存器 0: 压力 (实际值 × 100)
  - 寄存器 1: 温度 (实际值 × 10)
  - 寄存器 2: 水泵状态 (0 = 停, 1 = 运行)

物理逻辑：
  * 泵启动后：压力指数逼近目标值 ~1.30 MPa，并带 ±0.03 MPa 抖动
  * 泵停止后：压力按指数衰减 → 0（这就是你想要的"泄压曲线"）
  * 温度跟随泵状态：运行时缓慢升到 35°C，停止时缓慢回到 25°C

使用：
  1. 关闭 Modbus Slave 软件（502 端口不能被占用）
  2. pip install pymodbus
  3. 以管理员身份运行：python mock_slave.py
  4. 启动 PumpHMI
  5. 在本窗口输入命令：
     - alarm      固定压力为 2.00 MPa，直接测试超限报警
     - set 2.30   固定压力为指定值（单位 MPa）
     - set 2.30 45.0  同时固定压力和温度
     - temp 45.0  只固定温度（单位 °C）
     - auto       压力和温度都恢复自动模型
     - status     查看当前寄存器和控制模式
"""

import math
import random
import threading
import time

from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusServerContext,
    ModbusSlaveContext,
)
from pymodbus.server import StartTcpServer

# ===== 物理参数（可调）=====
TARGET_PRESSURE = 130   # 泵开时目标压力 1.30 MPa  → 寄存器值 130
RISE_RATE       = 0.20  # 压力上升的逼近系数（越大升得越快）
DECAY_RATE      = 0.85  # 压力衰减系数（每秒乘这个值，越小掉得越快）
PRESSURE_JITTER = 3     # 运行时随机抖动 ± 0.03 MPa
TEMP_RUN        = 350   # 运行时目标温度 35.0°C  → 寄存器值 350
TEMP_IDLE       = 250   # 停止时目标温度 25.0°C  → 寄存器值 250
TEMP_RATE       = 0.05  # 温度变化的逼近系数（升降都慢）
ALARM_TEST_PRESSURE = 200  # alarm 命令使用 2.00 MPa
MAX_MANUAL_PRESSURE_MPA = 3.00  # 与 HMI 图表量程保持一致
MAX_MANUAL_TEMPERATURE_C = 100.0

# Modbus 内部用 function code 3 表示 Holding Register
FC_HOLDING = 3

# 初始状态：压力 0、温度 25.0°C、泵停止
store = ModbusSlaveContext(
    hr=ModbusSequentialDataBlock(0, [0, TEMP_IDLE, 0]),
    zero_mode=True,
)
context = ModbusServerContext(slaves={1: store}, single=False)

state_lock = threading.Lock()
simulation_wakeup = threading.Event()
manual_pressure = None
manual_temperature = None


def pressure_to_register(pressure_mpa):
    """校验 MPa 值并转换为压力寄存器整数。"""
    if not math.isfinite(pressure_mpa) or not 0 <= pressure_mpa <= MAX_MANUAL_PRESSURE_MPA:
        raise ValueError("压力必须在 0～3.00 MPa 之间")
    return int(round(pressure_mpa * 100))


def temperature_to_register(temperature_c):
    """校验摄氏温度并转换为温度寄存器整数。"""
    if not math.isfinite(temperature_c) or not 0 <= temperature_c <= MAX_MANUAL_TEMPERATURE_C:
        raise ValueError("温度必须在 0～100.0 °C 之间")
    return int(round(temperature_c * 10))


def set_manual_pressure(pressure_mpa):
    """进入手动模式并立即写入压力，参数单位为 MPa。"""
    global manual_pressure

    register_value = pressure_to_register(pressure_mpa)

    with state_lock:
        manual_pressure = register_value
    simulation_wakeup.set()

    return register_value


def set_manual_temperature(temperature_c):
    """进入手动模式并保持指定温度，参数单位为 °C。"""
    global manual_temperature

    register_value = temperature_to_register(temperature_c)

    with state_lock:
        manual_temperature = register_value
    simulation_wakeup.set()

    return register_value


def set_manual_values(pressure_mpa, temperature_c):
    """原子地设置并保持压力与温度。"""
    global manual_pressure, manual_temperature

    pressure_value = pressure_to_register(pressure_mpa)
    temperature_value = temperature_to_register(temperature_c)

    with state_lock:
        manual_pressure = pressure_value
        manual_temperature = temperature_value
    simulation_wakeup.set()

    return pressure_value, temperature_value


def enable_auto_mode():
    """退出手动压力和温度保持，恢复自动物理模型。"""
    global manual_pressure, manual_temperature
    with state_lock:
        manual_pressure = None
        manual_temperature = None
    simulation_wakeup.set()


def show_status():
    """在控制台显示当前三个寄存器和压力控制模式。"""
    pressure, temperature, pump = store.getValues(FC_HOLDING, 0, count=3)
    with state_lock:
        current_manual_pressure = manual_pressure
        current_manual_temperature = manual_temperature

    pressure_mode = "自动" if current_manual_pressure is None else "手动"
    temperature_mode = "自动" if current_manual_temperature is None else "手动"

    print(
        f">>> 状态: 压力={pressure / 100:.2f} MPa, "
        f"温度={temperature / 10:.1f} °C, 水泵={'运行' if pump else '停止'}, "
        f"压力模式={pressure_mode}, 温度模式={temperature_mode}",
        flush=True,
    )


def handle_console_command(command):
    """处理一条控制台命令，供交互线程和测试复用。"""
    parts = command.strip().split()
    if not parts:
        return

    name = parts[0].lower()

    if name == "alarm":
        set_manual_pressure(ALARM_TEST_PRESSURE / 100)
        print(">>> 已进入手动模式：压力固定为 2.00 MPa，将触发超限报警", flush=True)
    elif name == "set":
        if len(parts) not in (2, 3):
            print(">>> 用法: set 2.30 或 set 2.30 45.0", flush=True)
            return
        try:
            if len(parts) == 2:
                pressure_value = set_manual_pressure(float(parts[1]))
                temperature_value = None
            else:
                pressure_value, temperature_value = set_manual_values(
                    float(parts[1]), float(parts[2])
                )
        except (ValueError, OverflowError):
            print(">>> 数值无效：压力范围 0～3.00 MPa，温度范围 0～100.0 °C", flush=True)
            return
        if temperature_value is None:
            print(
                f">>> 已进入手动模式：压力固定为 {pressure_value / 100:.2f} MPa",
                flush=True,
            )
        else:
            print(
                f">>> 已进入手动模式：压力固定为 {pressure_value / 100:.2f} MPa，"
                f"温度固定为 {temperature_value / 10:.1f} °C",
                flush=True,
            )
    elif name == "temp":
        if len(parts) != 2:
            print(">>> 用法: temp 45.0", flush=True)
            return
        try:
            temperature_value = set_manual_temperature(float(parts[1]))
        except (ValueError, OverflowError):
            print(">>> 温度值无效，请输入 0～100.0 之间的数字，例如: temp 45.0", flush=True)
            return
        print(
            f">>> 已进入手动模式：温度固定为 {temperature_value / 10:.1f} °C",
            flush=True,
        )
    elif name == "auto":
        enable_auto_mode()
        print(">>> 压力和温度均已恢复自动模型", flush=True)
    elif name == "status":
        show_status()
    elif name == "help":
        print(
            ">>> 命令: alarm | set <MPa> [°C] | temp <°C> | auto | status | help",
            flush=True,
        )
    else:
        print(">>> 未知命令，输入 help 查看可用命令", flush=True)


def console_control():
    """独立线程读取控制台命令，不阻塞 Modbus TCP 服务。"""
    while True:
        try:
            command = input("mock> ")
        except (EOFError, OSError):
            return
        handle_console_command(command)


def simulate():
    """每秒根据泵状态更新压力和温度。"""
    while True:
        simulation_wakeup.wait(timeout=1.0)
        simulation_wakeup.clear()

        pressure, temperature, pump_value = store.getValues(FC_HOLDING, 0, count=3)
        pump_on = pump_value != 0
        with state_lock:
            forced_pressure = manual_pressure
            forced_temperature = manual_temperature

        if forced_temperature is not None:
            new_temp = forced_temperature
        elif pump_on:
            new_temp = int(temperature + (TEMP_RUN - temperature) * TEMP_RATE)
        else:
            new_temp = int(temperature + (TEMP_IDLE - temperature) * TEMP_RATE)

        if forced_pressure is not None:
            # 手动模式：持续保持指定压力，不被自动模型覆盖
            new_pressure = forced_pressure
        elif pump_on:
            # 压力指数逼近目标 + 抖动
            new_pressure = pressure + (TARGET_PRESSURE - pressure) * RISE_RATE
            new_pressure += random.uniform(-PRESSURE_JITTER, PRESSURE_JITTER)
            new_pressure = max(0, int(new_pressure))
        else:
            # 泄压：指数衰减
            new_pressure = int(pressure * DECAY_RATE)

        store.setValues(FC_HOLDING, 0, [new_pressure, new_temp])


if __name__ == "__main__":
    print(">>> PumpHMI Mock Modbus Slave")
    print(">>> 监听 127.0.0.1:502 (Unit ID = 1)")
    print(">>> 寄存器: 0=压力 ×100, 1=温度 ×10, 2=泵状态 (0/1)")
    print(">>> 手动命令: alarm | set <MPa> [°C] | temp <°C> | auto | status | help")
    print(">>> Ctrl+C 退出")
    threading.Thread(target=simulate, daemon=True).start()
    threading.Thread(target=console_control, daemon=True).start()
    StartTcpServer(context=context, address=("127.0.0.1", 502))
