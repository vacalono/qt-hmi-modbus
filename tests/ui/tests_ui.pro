QT += widgets charts network serialport testlib
CONFIG += console c++17 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_ui

INCLUDEPATH += ../../src

SOURCES += \
    test_ui.cpp \
    ../../src/protocol/ModbusFrame.cpp \
    ../../src/transport/TcpTransport.cpp \
    ../../src/transport/SerialTransport.cpp \
    ../../src/transport/IoWorker.cpp \
    ../../src/transport/TransportChannel.cpp \
    ../../src/core/PollEngine.cpp \
    ../../src/core/CsvLogger.cpp \
    ../../src/core/ScanController.cpp \
    ../../src/core/QcChecker.cpp \
    ../../src/ui/MainWindow.cpp

HEADERS += \
    ../../src/protocol/ModbusFrame.h \
    ../../src/transport/ITransport.h \
    ../../src/transport/TcpTransport.h \
    ../../src/transport/SerialTransport.h \
    ../../src/transport/IoWorker.h \
    ../../src/transport/TransportChannel.h \
    ../../src/core/PollEngine.h \
    ../../src/core/CsvLogger.h \
    ../../src/core/ScanController.h \
    ../../src/core/QcChecker.h \
    ../../src/ui/MainWindow.h
