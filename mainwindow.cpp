#include "mainwindow.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPainter>
#include <QSqlError>
#include <QSqlQuery>
#include <QStatusBar>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUI();          // 必须先于 setupChart：图表会挂到 centralLayout 上
    setupChart();
    setupDatabase();
    connectModbus();
}

MainWindow::~MainWindow() = default;

// mainwindow.cpp — 界面构建
void MainWindow::setupUI()
{
    auto *central = new QWidget(this);
    setCentralWidget(central);
    centralLayout = new QVBoxLayout(central);

    // 顶部状态行：指示灯 + 压力数值 + 启停按钮
    auto *topRow = new QHBoxLayout();

    m_statusLight = new QLabel();
    m_statusLight->setStyleSheet(
        "background-color: #9E9E9E; border-radius: 12px;"
        "min-width: 24px; min-height: 24px;");

    m_pressureLabel = new QLabel("-- MPa");
    m_pressureLabel->setAlignment(Qt::AlignCenter);
    m_pressureLabel->setStyleSheet(
        "color: #2196F3; font-size: 32px; padding: 10px;");

    m_temperatureLabel = new QLabel("-- °C");
    m_temperatureLabel->setAlignment(Qt::AlignCenter);
    m_temperatureLabel->setStyleSheet(
        "color: #FF9800; font-size: 20px; padding: 10px;");

    m_pumpButton = new QPushButton("启动水泵");
    m_pumpButton->setStyleSheet(
        "background-color: #4CAF50; color: white; padding: 12px;");
    connect(m_pumpButton, &QPushButton::clicked,
            this, &MainWindow::togglePump);

    topRow->addWidget(m_statusLight);
    topRow->addWidget(m_pressureLabel, 2);
    topRow->addWidget(m_temperatureLabel, 1);
    topRow->addWidget(m_pumpButton);
    centralLayout->addLayout(topRow);

    // 日志表格
    m_logTable = new QTableWidget(0, 3);
    m_logTable->setHorizontalHeaderLabels({"时间", "压力 (MPa)", "信息"});
    m_logTable->horizontalHeader()->setStretchLastSection(true);
    m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    centralLayout->addWidget(m_logTable);

    setWindowTitle("水泵监控 HMI");
    resize(900, 600);
}

// mainwindow.cpp — 连接部分
void MainWindow::connectModbus()
{
    m_modbusClient = new QModbusTcpClient(this);

    // 连接到 Modbus Slave 模拟器
    m_modbusClient->setConnectionParameter(
        QModbusDevice::NetworkPortParameter, 502);
    m_modbusClient->setConnectionParameter(
        QModbusDevice::NetworkAddressParameter, "127.0.0.1");
    m_modbusClient->setTimeout(1000);       // 超时 1 秒
    m_modbusClient->setNumberOfRetries(3);  // 重试 3 次

    if (!m_modbusClient->connectDevice()) {
        statusBar()->showMessage(
            QString("连接下位机失败: %1").arg(m_modbusClient->errorString()));
        m_pumpButton->setEnabled(false);
        m_pumpButton->setText("未连接下位机");
        return;
    }

    statusBar()->showMessage("已连接到下位机 (127.0.0.1:502)", 3000);
    m_pumpButton->setEnabled(true);

    // 定时器创建但不立刻启动 —— 等点"启动水泵"才开始轮询
    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::readPressure);

    // 初始为"停机"状态：界面全部归零
    resetDisplay();
}

// mainwindow.cpp — 读取部分
void MainWindow::readPressure()
{
    if (!m_modbusClient || m_modbusClient->state() != QModbusDevice::ConnectedState)
        return;

    // 构建读取请求：从地址 0 开始，读取 3 个保持寄存器
    QModbusDataUnit readUnit(
        QModbusDataUnit::HoldingRegisters,  // 寄存器类型
        0,                                   // 起始地址
        3                                    // 读取数量
        );

    // 发送读取请求（异步）
    if (auto *reply = m_modbusClient->sendReadRequest(readUnit, 1)) {
        if (!reply->isFinished()) {
            connect(reply, &QModbusReply::finished,
                    this, &MainWindow::onReadReady);
        } else {
            delete reply;  // 广播请求，直接删除
        }
    }
}

void MainWindow::onReadReady()
{
    auto *reply = qobject_cast<QModbusReply *>(sender());
    if (!reply) return;

    if (reply->error() == QModbusDevice::NoError) {
        const QModbusDataUnit unit = reply->result();

        // 解析数据（寄存器值除以 100 得到实际值）
        float pressure = unit.value(0) / 100.0f;    // 地址 0：压力 (MPa)
        float temperature = unit.value(1) / 10.0f;  // 地址 1：温度 (°C)

        // 更新界面显示
        m_pressureLabel->setText(
            QString("%1 MPa").arg(pressure, 0, 'f', 2));
        m_temperatureLabel->setText(
            QString("%1 °C").arg(temperature, 0, 'f', 1));

        // 报警判定：超阈值触发，回落后清除
        if (pressure > m_alarmThreshold) {
            triggerAlarm(pressure);
        } else if (m_alarmActive) {
            m_alarmActive = false;
            applyNormalStyle();
        }

        // 更新趋势图
        updateChart(pressure);

    } else {
        statusBar()->showMessage(
            QString("读取失败: %1").arg(reply->errorString()), 2000);
    }

    reply->deleteLater();
}

// mainwindow.cpp — 图表初始化
void MainWindow::setupChart()
{
    m_series = new QLineSeries();
    m_series->setName("压力 (MPa)");
    m_series->setPen(QPen(QColor("#2196F3"), 2));

    m_chart = new QChart();
    m_chart->addSeries(m_series);
    m_chart->setTitle("实时压力趋势");
    m_chart->setAnimationOptions(QChart::NoAnimation); // 实时数据不要动画

    // X 轴：时间
    m_axisX = new QDateTimeAxis();
    m_axisX->setFormat("HH:mm:ss");
    m_axisX->setTitleText("时间");
    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_series->attachAxis(m_axisX);

    // Y 轴：压力值
    m_axisY = new QValueAxis();
    m_axisY->setRange(0, 3.0);
    m_axisY->setTitleText("压力 (MPa)");
    m_axisY->setLabelFormat("%.1f");
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisY);

    // 创建图表视图
    QChartView *chartView = new QChartView(m_chart);
    chartView->setRenderHint(QPainter::Antialiasing);

    centralLayout->addWidget(chartView, 1);
}

// mainwindow.cpp — 图表更新
void MainWindow::updateChart(float pressure)
{
    QDateTime now = QDateTime::currentDateTime();

    // 追加新数据点
    m_series->append(now.toMSecsSinceEpoch(), pressure);

    // 只保留最近 60 秒的数据（避免内存无限增长）
    QDateTime cutoff = now.addSecs(-60);
    while (m_series->count() > 0 &&
           m_series->at(0).x() < cutoff.toMSecsSinceEpoch()) {
        m_series->remove(0);
    }

    // 更新 X 轴范围：始终显示最近 60 秒
    m_axisX->setRange(cutoff, now);
}

// mainwindow.cpp — 报警逻辑
void MainWindow::triggerAlarm(float pressure)
{
    // 界面变红
    m_pressureLabel->setStyleSheet(
        "color: white; background-color: #F44336;"
        "font-size: 32px; padding: 10px; border-radius: 8px;");

    // 状态指示灯变红
    m_statusLight->setStyleSheet(
        "background-color: #F44336; border-radius: 12px;"
        "min-width: 24px; min-height: 24px;");

    // 只在首次超阈值时弹窗，避免反复弹
    if (!m_alarmActive) {
        m_alarmActive = true;
        QMessageBox::warning(this, "压力报警",
                             QString("当前压力 %1 MPa 超过阈值 %2 MPa！\n请立即检查水泵运行状态。")
                                 .arg(pressure, 0, 'f', 2)
                                 .arg(m_alarmThreshold, 0, 'f', 2));
    }

    // 记录到数据库
    logAlarm(pressure,
             QString("压力超阈值: %1 MPa > %2 MPa")
                 .arg(pressure, 0, 'f', 2)
                 .arg(m_alarmThreshold, 0, 'f', 2));
}

// 运行态样式：压力蓝、温度橙、状态灯绿
void MainWindow::applyNormalStyle()
{
    m_pressureLabel->setStyleSheet(
        "color: #2196F3; font-size: 32px; padding: 10px;");
    m_temperatureLabel->setStyleSheet(
        "color: #FF9800; font-size: 20px; padding: 10px;");
    m_statusLight->setStyleSheet(
        "background-color: #4CAF50; border-radius: 12px;"
        "min-width: 24px; min-height: 24px;");
}

// 停机/未启动时：把界面所有动态数据归零并置灰
void MainWindow::resetDisplay()
{
    m_pressureLabel->setText("0.00 MPa");
    m_pressureLabel->setStyleSheet(
        "color: #9E9E9E; font-size: 32px; padding: 10px;");

    m_temperatureLabel->setText("0.0 °C");
    m_temperatureLabel->setStyleSheet(
        "color: #9E9E9E; font-size: 20px; padding: 10px;");

    m_statusLight->setStyleSheet(
        "background-color: #9E9E9E; border-radius: 12px;"
        "min-width: 24px; min-height: 24px;");

    if (m_series) {
        m_series->clear();
    }
    m_alarmActive = false;
}

// mainwindow.cpp — 数据库初始化
void MainWindow::setupDatabase()
{
    m_db = QSqlDatabase::addDatabase("QSQLITE");
    m_db.setDatabaseName("pump_alarm_log.db");

    if (!m_db.open()) {
        qWarning() << "无法打开数据库:" << m_db.lastError().text();
        return;
    }

    // 创建报警日志表
    QSqlQuery query;
    query.exec(
        "CREATE TABLE IF NOT EXISTS alarm_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  pressure REAL,"
        "  message TEXT"
        ")"
        );
}

// mainwindow.cpp — 写入日志
void MainWindow::logAlarm(float pressure, const QString &message)
{
    // 写入数据库
    QSqlQuery query;
    query.prepare(
        "INSERT INTO alarm_log (pressure, message) VALUES (?, ?)");
    query.addBindValue(pressure);
    query.addBindValue(message);
    query.exec();

    // 同时更新界面上的日志表格
    int row = m_logTable->rowCount();
    m_logTable->insertRow(row);
    m_logTable->setItem(row, 0,
                        new QTableWidgetItem(
                            QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    m_logTable->setItem(row, 1,
                        new QTableWidgetItem(QString::number(pressure, 'f', 2)));
    m_logTable->setItem(row, 2,
                        new QTableWidgetItem(message));

    // 自动滚动到最新一条
    m_logTable->scrollToBottom();
}

// mainwindow.cpp — 控制水泵
void MainWindow::togglePump()
{
    if (!m_modbusClient || m_modbusClient->state() != QModbusDevice::ConnectedState) {
        QMessageBox::warning(this, "未连接下位机",
                             "Modbus 下位机未连接，无法控制水泵。\n"
                             "请确认 127.0.0.1:502 上有 Modbus Slave 在运行。");
        return;
    }

    bool nextState = !m_pumpRunning;

    // 构建写入请求：向地址 2 写入 1（启动）或 0（停止）
    QModbusDataUnit writeUnit(
        QModbusDataUnit::HoldingRegisters, 2, 1);
    writeUnit.setValue(0, nextState ? 1 : 0);

    if (auto *reply = m_modbusClient->sendWriteRequest(writeUnit, 1)) {
        connect(reply, &QModbusReply::finished, this, [this, reply, nextState]() {
            if (reply->error() == QModbusDevice::NoError) {
                m_pumpRunning = nextState;
                m_pumpButton->setText(m_pumpRunning ? "停止水泵" : "启动水泵");
                m_pumpButton->setStyleSheet(m_pumpRunning
                                                ? "background-color: #F44336; color: white; padding: 12px;"
                                                : "background-color: #4CAF50; color: white; padding: 12px;");
                if (m_pumpRunning) {
                    // 启动：恢复正常颜色，开始每秒轮询
                    applyNormalStyle();
                    m_pollTimer->start(1000);
                    statusBar()->showMessage("水泵已启动，开始监测", 2000);
                } else {
                    // 停止：停轮询并把界面全清零
                    m_pollTimer->stop();
                    resetDisplay();
                    statusBar()->showMessage("水泵已停止，监测已暂停", 2000);
                }
            } else {
                statusBar()->showMessage(
                    QString("写入失败: %1").arg(reply->errorString()), 2000);
            }
            reply->deleteLater();
        });
    }
}
