QT += core gui widgets network serialport charts
CONFIG += c++17
TEMPLATE = app
TARGET = hmi_app

INCLUDEPATH += src

SOURCES += \
    src/main.cpp \
    src/ui/MainWindow.cpp \
    src/protocol/ModbusFrame.cpp \
    src/transport/TcpTransport.cpp \
    src/transport/SerialTransport.cpp \
    src/transport/IoWorker.cpp \
    src/transport/TransportChannel.cpp \
    src/core/PollEngine.cpp \
    src/core/CsvLogger.cpp \
    src/core/ScanController.cpp \
    src/core/QcChecker.cpp

HEADERS += \
    src/ui/MainWindow.h \
    src/protocol/ModbusFrame.h \
    src/transport/ITransport.h \
    src/transport/TcpTransport.h \
    src/transport/SerialTransport.h \
    src/transport/IoWorker.h \
    src/transport/TransportChannel.h \
    src/core/PollEngine.h \
    src/core/CsvLogger.h \
    src/core/ScanController.h \
    src/core/QcChecker.h