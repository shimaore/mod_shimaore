include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_shimaore

mod_LTLIBRARIES = mod_shimaore.la
mod_shimaore_la_SOURCES  = mod_shimaore.c
mod_shimaore_la_CFLAGS   = $(AM_CFLAGS)
mod_shimaore_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_shimaore_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
