AM_CPPFLAGS = -fno-rtti -fno-exceptions -D__STDC_FORMAT_MACROS

AM_CPPFLAGS += \
	-I$(top_builddir) \
	-I$(top_srcdir) \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/zapit/include \
	-I$(top_srcdir)/lib \
	-I$(top_srcdir)/lib/libnet \
	-I$(top_srcdir)/lib/libconfigfile \
	@FREETYPE_CFLAGS@ \
	@SIGC_CFLAGS@

if BOXTYPE_COOL
if BOXMODEL_APOLLO
AM_CPPFLAGS += -I$(top_srcdir)/lib/libcoolstream2
else
AM_CPPFLAGS += -I$(top_srcdir)/lib/libcoolstream
endif
endif

if USE_STB_HAL
AM_CPPFLAGS += -I$(STB_HAL_INC)
endif

noinst_LIBRARIES = libneutrino_gui_widget.a libneutrino_gui_widget2.a

libneutrino_gui_widget_a_SOURCES = \
	buttons.cpp \
	colorchooser.cpp \
	drawable.cpp \
	helpbox.cpp \
	hintbox.cpp \
	hintboxext.cpp \
	keychooser.cpp \
	listbox.cpp \
	listframe.cpp \
	menue.cpp \
	messagebox.cpp \
	mountchooser.cpp \
	msgbox.cpp \
	shellwindow.cpp \
	stringinput.cpp \
	stringinput_ext.cpp \
	textbox.cpp

libneutrino_gui_widget2_a_SOURCES = \
	progresswindow.cpp
