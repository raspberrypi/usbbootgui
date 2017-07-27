bin_PROGRAMS = usbbootgui

usbbootgui_SOURCES = usbbootgui.c stealcookie.c stealcookie.h
usbbootgui_CFLAGS = $(USBBOOTGUI_CFLAGS)

usbbootgui_LDADD = $(USBBOOTGUI_LIBS) -lusb-1.0

#resource_files = $(shell glib-compile-resources --sourcedir=$(srcdir)/../data --generate-dependencies $(srcdir)/../data/usbbootgui.gresource.xml)
#usbbootgui-resources.c: ../data/usbbootgui.gresource.xml $(resource_files)
#	$(AM_V_GEN) glib-compile-resources --target=$@ --sourcedir=$(srcdir)/../data --generate-source --c-name usbbootgui $<
#usbbootgui-resources.h: ../data/usbbootgui.gresource.xml $(resource_files)
#	$(AM_V_GEN) glib-compile-resources --target=$@ --sourcedir=$(srcdir)/../data --generate-header --c-name usbbootgui $<

AM_CPPFLAGS = -DLOCALE_DIR='"$(localedir)"' -DPACKAGE_DATA_DIR='"$(datadir)/usbbootgui"'

-include $(top_srcdir)/git.mk
