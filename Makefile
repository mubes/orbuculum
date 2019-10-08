#VERBOSE=1
DEBUG=1
#WITH_FPGA?=1

CFLAGS=-DVERSION="\"1.00\""

CROSS_COMPILE=
# Output Files
ORBUCULUM = orbuculum
ORBCAT = orbcat
ORBTOP = orbtop
ORBDUMP = orbdump
ORBSTAT = orbstat

##########################################################################
# Check Host OS
##########################################################################

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  CFLAGS += -DLINUX
  LINUX=1
endif
ifeq ($(UNAME_S),Darwin)
  CFLAGS += -DOSX
  OSX=1
endif

##########################################################################
# User configuration and firmware specific object files	
##########################################################################

# Overall system defines for compilation
ifdef DEBUG
GCC_DEFINE= -DDEBUG
OPT_LEVEL = 
else
GCC_DEFINE=
OPT_LEVEL = -O2
endif

GCC_DEFINE+= -std=gnu99

CFILES =
SFILES =
OLOC = ofiles
INCLUDE_PATHS = -I/usr/local/include/libusb-1.0 -I/usr/include/libiberty
INCLUDE_PATHS += -I/mingw64/include -I/mingw64/include/libusb-1.0 -I /mingw64/include/libiberty
LDLIBS = -L/usr/lib -L/mingw64/lib -L/mingw64/lib/binutils -L/usr/local/lib -lusb-1.0 -lelf -lbfd -lz -ldl -liberty -lintl

#ifdef LINUX
LDLIBS += -lpthread
#endif

ifeq ($(WITH_FPGA),1)
CFLAGS+=-DINCLUDE_FPGA_SUPPORT
LDLIBS += -lftdi1
FPGA_CFILES=$(App_DIR)/ftdispi.c
endif

DEBUG_OPTS = -g3 -gdwarf-2 -ggdb

##########################################################################
# Generic multi-project files 
##########################################################################

##########################################################################
# Project-specific files 
##########################################################################

# Main Files
# ==========
App_DIR=Src
INCLUDE_PATHS += -IInc -I$(OLOC)

ORBUCULUM_CFILES = $(App_DIR)/$(ORBUCULUM).c $(App_DIR)/filewriter.c $(FPGA_CFILES)
ORBCAT_CFILES = $(App_DIR)/$(ORBCAT).c 
ORBTOP_CFILES = $(App_DIR)/$(ORBTOP).c $(App_DIR)/symbols.c 
ORBDUMP_CFILES = $(App_DIR)/$(ORBDUMP).c
ORBSTAT_CFILES = $(App_DIR)/$(ORBSTAT).c $(App_DIR)/symbols.c 

##########################################################################
# GNU GCC compiler prefix and location
##########################################################################

ASTYLE = astyle
AS = $(CROSS_COMPILE)gcc
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)gcc
GDB = $(CROSS_COMPILE)gdb
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
GET_GIT_HASH = Tools/git_hash_to_c/git_hash_to_c.sh
MAKE = make

##########################################################################
# Quietening
##########################################################################

ifdef VERBOSE
cmd = $1
Q :=
else
cmd = @$(if $(value 2),echo "$2";)$1
Q := @
endif

HOST=-lc -lusb

##########################################################################
# Compiler settings, parameters and flags
##########################################################################
# filename for embedded git revision 
GIT_HASH_FILENAME=git_version_info.h

CFLAGS +=  $(ARCH_FLAGS) $(STARTUP_DEFS) $(OPT_LEVEL) $(DEBUG_OPTS) \
		-ffunction-sections -fdata-sections -Wall -Wno-unused-result $(INCLUDE_PATHS)  $(GCC_DEFINE)
ASFLAGS += -c $(DEBUG_OPTS) $(INCLUDE_PATHS) $(ARCH_FLAGS) $(GCC_DEFINE) \
          -x assembler-with-cpp
LDFLAGS += $(CFLAGS)

OCFLAGS += --strip-unneeded

# Generic Stuff
OBJS =  $(patsubst %.c,%.o,$(CFILES)) $(patsubst %.s,%.o,$(SFILES))
POBJS = $(patsubst %,$(OLOC)/%,$(OBJS))
PDEPS = $(POBJS:.o=.d)

# Per Target Stuff
ORBUCULUM_OBJS =  $(OBJS) $(patsubst %.c,%.o,$(ORBUCULUM_CFILES))
ORBUCULUM_POBJS = $(POJBS) $(patsubst %,$(OLOC)/%,$(ORBUCULUM_OBJS))
ORBUCULUM_PDEPS = $(PDEPS) $(ORBUCULUM_POBJS:.o=.d)

ORBCAT_OBJS =  $(OBJS) $(patsubst %.c,%.o,$(ORBCAT_CFILES))
ORBCAT_POBJS = $(POJBS) $(patsubst %,$(OLOC)/%,$(ORBCAT_OBJS))
ORBCAT_PDEPS = $(PDEPS) $(ORBCAT_POBJS:.o=.d)

ORBTOP_OBJS =  $(OBJS) $(patsubst %.c,%.o,$(ORBTOP_CFILES))
ORBTOP_POBJS = $(POJBS) $(patsubst %,$(OLOC)/%,$(ORBTOP_OBJS))
ORBTOP_PDEPS = $(PDEPS) $(ORBTOP_POBJS:.o=.d)

ORBDUMP_OBJS =  $(OBJS) $(patsubst %.c,%.o,$(ORBDUMP_CFILES))
ORBDUMP_POBJS = $(POJBS) $(patsubst %,$(OLOC)/%,$(ORBDUMP_OBJS))
ORBDUMP_PDEPS = $(PDEPS) $(ORBDUMP_POBJS:.o=.d)

ORBSTAT_OBJS =  $(OBJS) $(patsubst %.c,%.o,$(ORBSTAT_CFILES))
ORBSTAT_POBJS = $(POJBS) $(patsubst %,$(OLOC)/%,$(ORBSTAT_OBJS))
ORBSTAT_PDEPS = $(PDEPS) $(ORBSTAT_POBJS:.o=.d)

CFILES += $(App_DIR)/itmDecoder.c $(App_DIR)/tpiuDecoder.c $(App_DIR)/generics.c

##########################################################################
##########################################################################
##########################################################################

all : build 

get_version:
	$(Q)mkdir -p $(OLOC)
	$(Q)$(GET_GIT_HASH) > $(OLOC)/$(GIT_HASH_FILENAME)

$(OLOC)/%.o : %.c
	$(Q)mkdir -p $(basename $@)
	$(call cmd, \$(CC) -c $(CFLAGS) -MMD -o $@ $< ,\
	Compiling $<)

build: $(ORBUCULUM) $(ORBCAT) $(ORBTOP) $(ORBDUMP) $(ORBSTAT)

$(ORBUCULUM) : get_version $(ORBUCULUM_POBJS) $(SYS_OBJS)
	$(Q)$(LD) $(LDFLAGS) -o $(OLOC)/$(ORBUCULUM) $(MAP) $(ORBUCULUM_POBJS) $(LDLIBS)
	-@echo "Completed build of" $(ORBUCULUM)

$(ORBCAT) : get_version $(ORBCAT_POBJS) $(SYS_OBJS)
	$(Q)$(LD) $(LDFLAGS) -o $(OLOC)/$(ORBCAT) $(MAP) $(ORBCAT_POBJS) $(LDLIBS)
	-@echo "Completed build of" $(ORBCAT)

$(ORBTOP) : get_version $(ORBTOP_POBJS) $(SYS_OBJS)
	$(Q)$(LD) $(LDFLAGS) -o $(OLOC)/$(ORBTOP) $(MAP) $(ORBTOP_POBJS) $(LDLIBS)
	-@echo "Completed build of" $(ORBTOP)

$(ORBDUMP) : get_version $(ORBDUMP_POBJS) $(SYS_OBJS)
	$(Q)$(LD) $(LDFLAGS) -o $(OLOC)/$(ORBDUMP) $(MAP) $(ORBDUMP_POBJS) $(LDLIBS)
	-@echo "Completed build of" $(ORBDUMP)

$(ORBSTAT) : get_version $(ORBSTAT_POBJS) $(SYS_OBJS)
	$(Q)$(LD) $(LDFLAGS) -o $(OLOC)/$(ORBSTAT) $(MAP) $(ORBSTAT_POBJS) $(LDLIBS)
	-@echo "Completed build of" $(ORBSTAT)

tags:
	-@etags $(CFILES) 2> /dev/null

clean:
	-$(call cmd, \rm -f $(POBJS) $(LD_TEMP) $(ORBUCULUM) $(ORBCAT) $(ORBDUMP) $(ORBSTAT) $(OUTFILE).map $(EXPORT) ,\
	Cleaning )
	$(Q)-rm -rf SourceDoc/*
	$(Q)-rm -rf *~ core
	$(Q)-rm -rf $(OLOC)/*
	$(Q)-rm -rf config/*~
	$(Q)-rm -rf TAGS

$(generated_dir)/git_head_revision.c:
	mkdir -p $(dir $@)
	../Tools/git_hash_to_c.sh > $@    

doc:
	doxygen $(DOXCONFIG)

print-%:
	@echo $* is $($*)

pretty:
	$(Q)-$(ASTYLE) --options=config/astyle.conf "Inc/*.h" "Src/*.c"

-include $(PDEPS)
