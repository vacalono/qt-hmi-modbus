QT += testlib core network serialport
CONFIG += console c++17 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_modbus

INCLUDEPATH += ../src

SOURCES += \
    test_modbus.cpp \
    ../src/protocol/ModbusFrame.cpp \
    ../src/transport/TcpTransport.cpp \
    ../src/transport/SerialTransport.cpp \
    ../src/transport/IoWorker.cpp \
    ../src/transport/TransportChannel.cpp

HEADERS += \
    ../src/protocol/ModbusFrame.h \
    ../src/transport/ITransport.h \
    ../src/transport/TcpTransport.h \
    ../src/transport/SerialTransport.h \
    ../src/transport/IoWorker.h \
    ../src/transport/TransportChannel.h
