SHARED_LIB = ${LIB_PREFIX}audqt${LIB_SUFFIX}
LIB_MAJOR = 2
LIB_MINOR = 2

SRCS = about-qt.cc \
       art-qt.cc \
       audqt.cc \
       eq-preset-qt.cc \
       equalizer-qt.cc \
       file-entry.cc \
       fileopener.cc \
       images.cc \
       infopopup-qt.cc \
       infowin-qt.cc \
       info-widget.cc \
       log-inspector.cc \
       menu-qt.cc \
       playlist-management.cc \
       plugin-menu-qt.cc \
       prefs-builder.cc \
       prefs-plugin.cc \
       prefs-widget-qt.cc \
       prefs-window-qt.cc \
       prefs-pluginlist-model.cc \
       queue-manager-qt.cc \
       url-opener-qt.cc \
       util-qt.cc \
       treeview.cc \
       volumebutton.cc

INCLUDES = export.h \
           iface.h \
           info-widget.h \
           libaudqt.h \
           menu.h \
           treeview.h

include ../../buildsys.mk
include ../../extra.mk

includesubdir = libaudqt

LD = ${CXX}

CPPFLAGS := -I.. -I../.. \
            ${CPPFLAGS} \
            ${QT_CFLAGS} \
            -DLIBAUDQT_BUILD

CFLAGS += ${LIB_CFLAGS}

LIBS := -L../libaudcore -laudcore \
        ${LIBS} -lm \
        ${QT_LIBS}

images.cc: images.qrc
	${QT_BINPATH}/rcc images.qrc -o images.cc
