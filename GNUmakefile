lsb_dist     := $(shell if [ -x /usr/bin/lsb_release ] ; then lsb_release -is ; else uname -s ; fi)
lsb_dist_ver := $(shell if [ -x /usr/bin/lsb_release ] ; then lsb_release -rs | sed 's/[.].*//' ; else uname -r | sed 's/[-].*//' ; fi)
uname_m      := $(shell uname -m)

short_dist_lc := $(patsubst CentOS,rh,$(patsubst RedHatEnterprise,rh,\
                   $(patsubst RedHat,rh,\
                     $(patsubst Fedora,fc,$(patsubst Ubuntu,ub,\
                       $(patsubst Debian,deb,$(patsubst SUSE,ss,$(lsb_dist))))))))
short_dist    := $(shell echo $(short_dist_lc) | tr a-z A-Z)
pwd           := $(shell pwd)
rpm_os        := $(short_dist_lc)$(lsb_dist_ver).$(uname_m)

# this is where the targets are compiled
build_dir ?= $(short_dist)$(lsb_dist_ver)_$(uname_m)$(port_extra)
bind      := $(build_dir)/bin
libd      := $(build_dir)/lib64
objd      := $(build_dir)/obj
dependd   := $(build_dir)/dep

# use 'make port_extra=-g' for debug build
ifeq (-g,$(findstring -g,$(port_extra)))
  DEBUG = true
endif

CC          ?= gcc
CXX         ?= $(CC) -x c++
cc          := $(CC) -std=c11
cpp         := $(CXX)
arch_cflags := -fno-omit-frame-pointer
gcc_wflags  := -Wall -Wextra -Werror
fpicflags   := -fPIC
soflag      := -shared

ifdef DEBUG
default_cflags := -ggdb
else
default_cflags := -ggdb -O3
endif
# rpmbuild uses RPM_OPT_FLAGS
ifeq ($(RPM_OPT_FLAGS),)
CFLAGS ?= $(default_cflags)
else
CFLAGS ?= $(RPM_OPT_FLAGS)
endif
cflags := $(gcc_wflags) $(CFLAGS) $(arch_cflags)

INCLUDES   ?= -Iinclude
DEFINES    ?=
includes   := $(INCLUDES)
defines    := $(DEFINES)
# if not linking libstdc++
ifdef NO_STL
cppflags   := -std=c++11 -fno-rtti -fno-exceptions
cpplink    := $(CC)
else
cppflags   := -std=c++11
cpplink    := $(CXX)
endif
#cppflags  := -fno-rtti -fno-exceptions -fsanitize=address
#cpplink   := $(CC) -lasan
cclink     := $(CC)

cpp_lnk    :=
lnk_lib    :=

have_lc_submodule := $(shell if [ -f ./linecook/GNUmakefile ]; then echo yes; else echo no; fi )

ifeq (yes,$(have_lc_submodule))
lc_lib      := linecook/$(libd)/liblinecook.a
lc_dll      := linecook/$(libd)/liblinecook.so
lnk_lib     += $(lc_lib) -lpcre2-32
lnk_dep     += $(lc_lib)
dlnk_lib    += -Llinecook/$(libd) -llinecook
dlnk_dep    += $(lc_dll)
else
lnk_lib     += -llinecook
dlnk_lib    += -llinecook
endif

.PHONY: everything
everything: $(lc_lib) all

ifeq (yes,$(have_lc_submodule))
$(lc_lib) $(lc_dll):
	$(MAKE) -C linecook
.PHONY: clean_lc
clean_lc:
	$(MAKE) -C linecook clean
clean_subs += clean_lc
update_submod:
	git update-index --cacheinfo 160000 `cd ./linecook && git rev-parse HEAD` linecook
endif

# copr/fedora build (with version env vars)
# copr uses this to generate a source rpm with the srpm target
-include .copr/Makefile

# debian build (debuild)
# target for building installable deb: dist_dpkg
-include deb/Makefile

all_exes    :=
all_depends :=

lcirc_files := lc_irc
lcirc_cfile := src/lc_irc.cpp
lcirc_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(lcirc_files)))
lcirc_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(lcirc_files)))
lcirc_lnk   := $(lnk_lib)

$(bind)/lcirc: $(lcirc_objs) $(lcirc_libs)

all_exes    += $(bind)/lcirc
all_depends += $(lcirc_deps)

all_dirs := $(bind) $(libd) $(objd) $(dependd)

all: $(all_libs) $(all_dlls) $(all_exes) cmake

.PHONY: cmake
cmake: CMakeLists.txt

.ONESHELL: CMakeLists.txt
CMakeLists.txt: .copr/Makefile
	@cat <<'EOF' > $@
	cmake_minimum_required (VERSION 3.9.0)
	if (POLICY CMP0111)
	  cmake_policy(SET CMP0111 OLD)
	endif ()
	project (lc_rc)
	include_directories (
	  include
	  $${CMAKE_SOURCE_DIR}/linecook/include
	  $${CMAKE_SOURCE_DIR}/pcre2
	)
	if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	  add_definitions(/DPCRE2_STATIC)
	  if ($$<CONFIG:Release>)
	    add_compile_options (/arch:AVX2 /GL /std:c11 /wd5105)
	  else ()
	    add_compile_options (/arch:AVX2 /std:c11 /wd5105)
	  endif ()
	  if (NOT TARGET pcre2-32-static)
	    add_library (pcre2-32-static STATIC IMPORTED)
	    set_property (TARGET pcre2-32-static PROPERTY IMPORTED_LOCATION_DEBUG ../pcre2/build/Debug/pcre2-32-staticd.lib)
	    set_property (TARGET pcre2-32-static PROPERTY IMPORTED_LOCATION_RELEASE ../pcre2/build/Release/pcre2-32-static.lib)
	    include_directories (../pcre2/build)
	  else ()
	    include_directories ($${CMAKE_BINARY_DIR}/pcre2)
	  endif ()
	  if (NOT TARGET linecook)
	    add_library (linecook STATIC IMPORTED)
	    set_property (TARGET linecook PROPERTY IMPORTED_LOCATION_DEBUG ../linecook/build/Debug/linecook.lib)
	    set_property (TARGET linecook PROPERTY IMPORTED_LOCATION_RELEASE ../linecook/build/Release/linecook.lib)
	  endif ()
	else ()
	  add_compile_options ($(cflags))
	  if (TARGET pcre2-32-static)
	    include_directories ($${CMAKE_BINARY_DIR}/pcre2)
	  endif ()
	  if (NOT TARGET linecook)
	    add_library (linecook STATIC IMPORTED)
	    set_property (TARGET linecook PROPERTY IMPORTED_LOCATION ../linecook/build/liblinecook.a)
	  endif ()
	endif ()
	if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	  set (ws_lib ws2_32)
	endif ()
	if (TARGET pcre2-32-static)
	  link_libraries (linecook pcre2-32-static $${ws_lib})
	else ()
	  link_libraries (linecook -lpcre2-32)
	endif ()
	add_executable (lcirc $(lcirc_cfile))
	EOF

# create directories
$(dependd):
	@mkdir -p $(all_dirs)

# remove target bins, objs, depends
.PHONY: clean
clean: $(clean_subs)
	rm -r -f $(bind) $(libd) $(objd) $(dependd)
	if [ "$(build_dir)" != "." ] ; then rmdir $(build_dir) ; fi

.PHONY: clean_dist
clean_dist:
	rm -rf dpkgbuild rpmbuild

.PHONY: clean_all
clean_all: clean clean_dist

$(dependd)/depend.make: $(dependd) $(all_depends)
	@echo "# depend file" > $(dependd)/depend.make
	@cat $(all_depends) >> $(dependd)/depend.make

ifeq (SunOS,$(lsb_dist))
remove_rpath = rpath -r
else
ifeq (Darwin,$(lsb_dist))
remove_rpath = true
else
remove_rpath = chrpath -d
endif
endif
# target used by rpmbuild, dpkgbuild
.PHONY: dist_bins
dist_bins: $(bind)/lcirc
	$(remove_rpath) $(bind)/lcirc

# target for building installable rpm
.PHONY: dist_rpm
dist_rpm: srpm
	( cd rpmbuild && rpmbuild --define "-topdir `pwd`" -ba SPECS/lcirc.spec )

# force a remake of depend using 'make -B depend'
.PHONY: depend
depend: $(dependd)/depend.make

# dependencies made by 'make depend'
-include $(dependd)/depend.make

ifeq ($(DESTDIR),)
# 'sudo make install' puts things in /usr/local/lib, /usr/local/include
install_prefix ?= /usr/local
else
# debuild uses DESTDIR to put things into debian/libdecnumber/usr
install_prefix = $(DESTDIR)/usr
endif
# this should be 64 for rpm based, /64 for SunOS
install_lib_suffix ?=

install: dist_bins
	install -d $(install_prefix)/bin
	install $(bind)/lcirc $(install_prefix)/bin

$(objd)/%.o: src/%.cpp
	$(cpp) $(cflags) $(cppflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(objd)/%.fpic.o: src/%.cpp
	$(cpp) $(cflags) $(fpicflags) $(cppflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(bind)/%:
	$(cpplink) $(cflags) $(rpath) -o $@ $($(*)_objs) $($(*)_lnk) $(cpp_lnk) $(sock_lib) $(math_lib) $(thread_lib) $(malloc_lib) $(dynlink_lib)

$(dependd)/%.d: src/%.cpp
	$(cpp) $(arch_cflags) $(defines) $(includes) $($(notdir $*)_includes) $($(notdir $*)_defines) -MM $< -MT $(objd)/$(*).o -MF $@

