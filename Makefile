# Native Linux CivNexus6 + Civ VI cooker.
# civnexus6 needs https://github.com/arves100/opengr2 (MPL-2.0) built as libopengrn.a.
OPENGR2 ?= ../opengr2
CC ?= gcc
CFLAGS ?= -O2 -Wall

.PHONY: all clean
all: civnexus6 civ6cook

civnexus6: src/civnexus6.c
	$(CC) $(CFLAGS) -o $@ $< -I$(OPENGR2)/libopengrn $(OPENGR2)/libopengrn.a -lm

civ6cook: src/civ6cook.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f civnexus6 civ6cook
