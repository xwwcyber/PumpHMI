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
"""

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

# Modbus 内部用 function code 3 表示 Holding Register
FC_HOLDING = 3

# 初始状态：压力 0、温度 25.0°C、泵停止
store = ModbusSlaveContext(
    hr=ModbusSequentialDataBlock(0, [0, TEMP_IDLE, 0])
)
context = ModbusServerContext(slaves={1: store}, single=False)


def simulate():
    """每秒根据泵状态更新压力和温度。"""
    while True:
        time.sleep(1)

        pressure = store.getValues(FC_HOLDING, 0, count=1)[0]
        temperature = store.getValues(FC_HOLDING, 1, count=1)[0]
        pump_on = store.getValues(FC_HOLDING, 2, count=1)[0] != 0

        if pump_on:
            # 压力指数逼近目标 + 抖动
            new_pressure = pressure + (TARGET_PRESSURE - pressure) * RISE_RATE
            new_pressure += random.uniform(-PRESSURE_JITTER, PRESSURE_JITTER)
            new_pressure = max(0, int(new_pressure))
            new_temp = int(temperature + (TEMP_RUN - temperature) * TEMP_RATE)
        else:
            # 泄压：指数衰减
            new_pressure = int(pressure * DECAY_RATE)
            new_temp = int(temperature + (TEMP_IDLE - temperature) * TEMP_RATE)

        store.setValues(FC_HOLDING, 0, [new_pressure])
        store.setValues(FC_HOLDING, 1, [new_temp])


if __name__ == "__main__":
    threading.Thread(target=simulate, daemon=True).start()
    print(">>> PumpHMI Mock Modbus Slave")
    print(">>> 监听 127.0.0.1:502 (Unit ID = 1)")
    print(">>> 寄存器: 0=压力 ×100, 1=温度 ×10, 2=泵状态 (0/1)")
    print(">>> Ctrl+C 退出")
    StartTcpServer(context=context, address=("127.0.0.1", 502))
