MAKE = make

ACLOCAL_AMFLAGS = -I ./build-aux

SUBDIRS =

# Driver
if OMX_BUILD_DRIVER
  SUBDIRS += driver/linux
endif

# Library
if OMX_BUILD_LIBRARY
  SUBDIRS += libopen-mx tools tests tests/mx
endif


# Public and private API
include_HEADERS = common/open-mx.h
if OMX_MX_ABI_COMPAT
include_HEADERS += common/mx/mx_extensions.h common/mx/mx_raw.h common/mx/myriexpress.h
endif
noinst_HEADERS  = common/omx_io.h common/omx_list.h common/omx_wire.h			\
		  common/mx/translate/mx_extensions.h common/mx/translate/myriexpress.h


# Man pages and other documentations
dist_man1_MANS    = doc/man/omx_counters.1 doc/man/omx_hostname.1 doc/man/omx_init_peers.1	\
		    doc/man/omx_prepare_binding.1 doc/man/omx_endpoint_info.1			\
	   	    doc/man/omx_info.1 doc/man/omx_perf.1
dist_pkgdata_DATA = doc/FAQ.html


EXTRA_DIST = AUTHORS ChangeLog COPYING COPYING.GPL COPYING.LGPL	\
	     README README.devel REPORTING-BUGS TODO open-mx.spec


if OMX_BUILD_LIBRARY
if OMX_USE_FMA
all-local: fma
clean-local: fma-clean
distclean-local: fma-distclean
install-exec-local: fma-install
endif
endif

.PHONY: fma fma-clean fma-distclean

fma:
	+$(MAKE) -C fma CC="$(CC)" MYRI_INC=-I$(abs_top_srcdir)/common/mx MYRI_LIB=$(abs_top_builddir)/libopen-mx/$(DEFAULT_LIBDIR)/.libs

fma-install:
	+$(MAKE) -C fma CC="$(CC)" MYRI_INC=-I$(abs_top_srcdir)/common/mx MYRI_LIB=$(abs_top_builddir)/libopen-mx/$(DEFAULT_LIBDIR)/.libs install
	
fma-clean:
	+$(MAKE) -C fma CC="$(CC)" MYRI_INC=-I$(abs_top_srcdir)/common/mx MYRI_LIB=$(abs_top_builddir)/libopen-mx/$(DEFAULT_LIBDIR)/.libs clean

fma-distclean:
	+$(MAKE) -C fma CC="$(CC)" MYRI_INC=-I$(abs_top_srcdir)/common/mx MYRI_LIB=$(abs_top_builddir)/libopen-mx/$(DEFAULT_LIBDIR)/.libs distclean
