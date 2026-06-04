#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QModbusTcpClient>
#include <QModbusDataUnit>
#include <QTimer>
#include <QtCharts>
#include <QSqlDatabase>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

// Qt5 下 QtCharts 类位于 QtCharts 命名空间；Qt6 下该宏为空
//QT_CHARTS_USE_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void connectModbus();              // 连接下位机
    void readPressure();               // 定时读取压力数据
    void onReadReady();                // 读取完成回调
    void triggerAlarm(float pressure); // 触发报警
    void togglePump();                 // 启停水泵

private:
    // Modbus 通信
    QModbusTcpClient *m_modbusClient = nullptr;
    QTimer *m_pollTimer = nullptr;

    // 实时图表
    QChart *m_chart = nullptr;
    QLineSeries *m_series = nullptr;
    QDateTimeAxis *m_axisX = nullptr;
    QValueAxis *m_axisY = nullptr;

    // 数据库
    QSqlDatabase m_db;

    // 界面元素
    QLabel *m_pressureLabel = nullptr;    // 压力数值显示
    QLabel *m_temperatureLabel = nullptr; // 温度数值显示
    QLabel *m_statusLight = nullptr;      // 状态指示灯
    QPushButton *m_pumpButton = nullptr;  // 启停按钮
    QTableWidget *m_logTable = nullptr;   // 日志表格
    QVBoxLayout *centralLayout = nullptr; // 主布局，setupChart 用来挂图表

    // 报警阈值与状态
    float m_alarmThreshold = 1.50f; // 压力超过 1.50 MPa 报警
    bool m_pumpRunning = false;
    bool m_alarmActive = false;

    void setupUI();
    void setupChart();
    void setupDatabase();
    void updateChart(float pressure);
    void applyNormalStyle();
    void resetDisplay();
    void logAlarm(float pressure, const QString &message);
};
#endif // MAINWINDOW_H
