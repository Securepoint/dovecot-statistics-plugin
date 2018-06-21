EXTRA_SRC = ../plugin-utils/plugin-utils.c
LOCAL_LIB_INCLUDE = /usr/include/dovecot
LOCAL_LIB =
LIB_NAME = lib20_uma_statistic_plugin
HEADER =
VER_FILE =
LIB_NAME_VER_LEVEL = 2
BUILDROOT = ../../build
SRC_RELATIVE_DIR = uma-statistic-plugin
BUILD_DYNAMIC = 1
BUILD_STATIC = 0
PREFIX = /usr/local
INSTALL_OWNER =
INSTALL_GROUP =
DEBUG = 1
CFLAGS += -Wall -fPIC -DHAVE_CONFIG_H `pkg-config --cflags tokyotyrant`
LDFLAGS = -L/usr/lib/dovecot
STATIC_LIB =
DYNAMIC_LIB = -ldovecot -ldovecot-lda -ldovecot-storage `pkg-config --libs tokyotyrant`

###############################################

SHELL = /bin/sh
CC = gcc
LD = gcc
AR = ar
INSTALL = install

VER_FILE_REAL = $(wildcard $(VER_FILE))
BUILDDIR = $(BUILDROOT)/$(SRC_RELATIVE_DIR)
BUILDDIR_BIN = $(BUILDROOT)/bin
LOCAL_LIB_BUILD = $(addprefix $(BUILDDIR_BIN)/, $(LOCAL_LIB))
TEMPDEP_SUFFIX = ~
LIB_SUBDIR = lib
LIBDIR = $(PREFIX)/$(LIB_SUBDIR)/dovecot
DESTLIBDIR = $(DESTDIR)$(LIBDIR)
INCLUDEDIR = $(PREFIX)/include
DESTINCLUDEDIR = $(DESTDIR)$(INCLUDEDIR)
SO_NAME = $(LIB_NAME).so
SO_BIN = $(BUILDDIR_BIN)/$(SO_NAME)$(LIB_NAME_VER_SUFFIX)
A_NAME = $(LIB_NAME).a
A_BIN = $(BUILDDIR_BIN)/$(A_NAME)
EXTRA_SRC_REAL = $(wildcard $(EXTRA_SRC))
SRC = $(wildcard *.c)
ASM = $(patsubst %.c,$(BUILDDIR)/%.s,$(SRC))
ASM+= $(patsubst %.c,$(BUILDDIR)/%.s,$(EXTRA_SRC_REAL))
OBJ = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRC))
OBJ+= $(patsubst %.c,$(BUILDDIR)/%.o,$(EXTRA_SRC_REAL))
DEP = $(patsubst %.c,$(BUILDDIR)/%.d,$(SRC))
DEP+= $(patsubst %.c,$(BUILDDIR)/%.d,$(EXTRA_SRC_REAL))

ifneq ($(VER_FILE_REAL), )
  LIB_VER = $(shell head -n 1 $(VER_FILE))
endif
ifneq ($(LIB_VER), )
  LIB_NAME_VER_SUFFIX = .$(shell echo $(LIB_VER) | cut -d . -f1-$(LIB_NAME_VER_LEVEL))
  LIB_SO_VER_SUFFIX = .$(LIB_VER)
endif
ifneq ($(LIB_SO_VER_SUFFIX), )
  ifneq ($(LIB_SO_VER_SUFFIX), $(LIB_NAME_VER_SUFFIX))
    INSTALL_LINK_NAME_CMD = ln -sf $(SO_NAME)$(LIB_SO_VER_SUFFIX) $(DESTLIBDIR)/$(SO_NAME)$(LIB_NAME_VER_SUFFIX)
    UNINSTALL_LINK_NAME += $(SO_NAME)$(LIB_NAME_VER_SUFFIX)
  endif
  INSTALL_LINK_SO_CMD = ln -sf $(SO_NAME)$(LIB_NAME_VER_SUFFIX) $(DESTLIBDIR)/$(SO_NAME)
  UNINSTALL_LINK_NAME += $(SO_NAME)$(LIB_SO_VER_SUFFIX)
endif
ifneq ($(INSTALL_OWNER), )
  INSTALL_OPTION += -o $(INSTALL_OWNER)
endif
ifneq ($(INSTALL_GROUP), )
  INSTALL_OPTION += -g $(INSTALL_GROUP)
endif
ifneq ($(LOCAL_LIB_INCLUDE), )
  CFLAGS += $(addprefix -I, $(LOCAL_LIB_INCLUDE))
endif
ifeq ($(DEBUG), 1)
  CFLAGS += -g -DDEBUG
endif

ifeq ($(MAKECMDGOALS), )
  MAKETARGETS = all
else
  MAKETARGETS = $(MAKECMDGOALS)
endif
ifeq ($(MAKE_RESTARTS), )
  $(info Making target "$(MAKETARGETS)" in directory "$(CURDIR)" ...)
endif

.PHONY: all install install_header install_so install_a uninstall uninstall_header uninstall_so uninstall_a clean

all: $(SO_BIN) $(A_BIN)

ifeq ($(MAKETARGETS), all)
$(BUILDDIR)/%.d: %.c
	@$(INSTALL) -d -m 755 $(BUILDDIR)/$(dir $*.d)
	@$(CC) $(CFLAGS) -M -MF $(BUILDDIR)/$*.d$(TEMPDEP_SUFFIX) -MQ $(BUILDDIR)/$*.o $<
	@echo "	$(CC) $(CPPFLAGS) $(CFLAGS) -S $< -o $(BUILDDIR)/$*.s" >> $(BUILDDIR)/$*.d$(TEMPDEP_SUFFIX)
	@echo "	$(CC) $(CPPFLAGS) $(CFLAGS) -c $(BUILDDIR)/$*.s -o $(BUILDDIR)/$*.o" >> $(BUILDDIR)/$*.d$(TEMPDEP_SUFFIX)
	@mv $(BUILDDIR)/$*.d$(TEMPDEP_SUFFIX) $(BUILDDIR)/$*.d
else
$(shell $(foreach DEP_FILE, $(DEP), install -d -m 755 $(dir $(DEP_FILE)); touch $(DEP_FILE);))
endif

$(SO_BIN): $(OBJ) $(LOCAL_LIB_BUILD)
ifeq ($(BUILD_DYNAMIC), 1)
	@$(INSTALL) -d -m 755 $(dir $(SO_BIN))
	$(LD) $(LDFLAGS) -shared -Wl,-soname,$(SO_NAME)$(LIB_NAME_VER_SUFFIX) -o $(SO_BIN) $(OBJ) $(LOCAL_LIB_BUILD) -Wl,-Bstatic $(STATIC_LIB) -Wl,-Bdynamic $(DYNAMIC_LIB)
endif

$(A_BIN): $(OBJ) $(LOCAL_LIB_BUILD)
ifeq ($(BUILD_STATIC), 1)
	@$(INSTALL) -d -m 755 $(dir $(A_BIN))
	$(AR) crs $(A_BIN) $(OBJ) $(LOCAL_LIB_BUILD)
endif

install: install_header install_so install_a

install_header:
ifneq ($(HEADER), )
	@$(INSTALL) -d -m 755 $(DESTINCLUDEDIR)
	$(INSTALL) $(INSTALL_OPTION) -m 644 $(HEADER) $(DESTINCLUDEDIR)
endif

install_so:
ifeq ($(BUILD_DYNAMIC), 1)
	@$(INSTALL) -d -m 755 $(DESTLIBDIR)
	$(INSTALL) $(INSTALL_OPTION) -m 755 $(SO_BIN) $(DESTLIBDIR)/$(SO_NAME)$(LIB_SO_VER_SUFFIX)
	$(INSTALL_LINK_NAME_CMD)
	$(INSTALL_LINK_SO_CMD)
endif

install_a:
ifeq ($(BUILD_STATIC), 1)
	@$(INSTALL) -d -m 755 $(DESTLIBDIR)
	$(INSTALL) $(INSTALL_OPTION) -m 755 $(A_BIN) $(DESTLIBDIR)/$(A_NAME)
endif

uninstall: uninstall_header uninstall_so uninstall_a

uninstall_header:
ifneq ($(HEADER), )
	rm -f $(addprefix $(DESTINCLUDEDIR)/, $(HEADER))
endif

uninstall_so:
ifeq ($(BUILD_DYNAMIC), 1)
	rm -f $(addprefix $(DESTLIBDIR)/, $(SO_NAME) $(UNINSTALL_LINK_NAME))
endif

uninstall_a:
ifeq ($(BUILD_STATIC), 1)
	rm -f $(addprefix $(DESTLIBDIR)/, $(A_NAME))
endif

clean:
	rm -f $(DEP) $(addsuffix $(TEMPDEP_SUFFIX), $(DEP)) $(ASM) $(OBJ) $(SO_BIN) $(A_BIN)

-include $(DEP)
