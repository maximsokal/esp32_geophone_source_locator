PRU_CGT ?= /usr/share/ti/cgt-pru
CLPRU   := $(PRU_CGT)/bin/clpru

CFLAGS  := -v3 --c99 -O2 --endian=little -Iinclude -I$(PRU_CGT)/include
LDFLAGS := -z --entry_point=_c_int00 -i$(PRU_CGT)/lib -llibc.a

TARGET  := build/pb2_pru1_ad7606.out
SRCS    := acquisition/main_pru1.c acquisition/pru1_ad7606.c
CMD     := linker/AM6232_PRU1.cmd

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) $(CMD) include/pru_adc_shared.h include/resource_table_empty.h
	@mkdir -p build
	$(CLPRU) $(CFLAGS) $(SRCS) $(LDFLAGS) $(CMD) -o $@

clean:
	rm -rf build
