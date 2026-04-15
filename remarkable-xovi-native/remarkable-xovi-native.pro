TEMPLATE = lib
TARGET = remarkable-xovi-native
CONFIG += shared plugin no_plugin_name_prefix

xoviextension.target = xovi.cpp
xoviextension.commands = python3 $$(XOVI_REPO)/util/xovigen.py -o xovi.cpp -H xovi.h remarkable-xovi-native.xovi
xoviextension.depends = remarkable-xovi-native.xovi

QMAKE_EXTRA_TARGETS += xoviextension
PRE_TARGETDEPS += xovi.cpp

QT += quick qml
CONFIG += c++17

SOURCES += src/main.cpp xovi.cpp

QMAKE_CXXFLAGS += -fPIC
