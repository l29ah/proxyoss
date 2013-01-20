CFLAGS += -Wall -Wextra -std=gnu99
CFLAGS += $(shell pkg-config fuse --cflags) -pthread
LDFLAGS += $(shell pkg-config fuse --libs)
include /etc/oss.conf
CFLAGS += -I$(OSSLIBDIR)/include/sys

all: proxyoss

proxyoss: proxyoss.o

.PHONY:	clean

clean:
	rm -rf *.o
	rm -rf proxyoss
