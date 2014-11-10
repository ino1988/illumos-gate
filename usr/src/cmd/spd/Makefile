#
# Copyright (c) 2014, Tegile Systems, Inc. All rights reserved.
#
PROG=		spd

include ../Makefile.cmd

OBJS = spd.o
SRCS = $(OBJS:.o=.c)

CFLAGS += -xc99=%all

LDLIBS += -lsmbios

.KEEP_STATE:

all: $(PROG)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
	$(POST_PROCESS)

install: all $(ROOTUSRSBINPROG)

check:	$(PROG).c
	$(CSTYLE) -pP $(SRCS:%=%)

clean:
	$(RM) $(OBJS) 

lint:	lint_SRCS

include ../Makefile.targ
