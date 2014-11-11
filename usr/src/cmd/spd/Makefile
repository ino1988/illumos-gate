#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

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
