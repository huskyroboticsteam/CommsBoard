# Host tests (gcc or clang on a PC). Run with: make -C tests
#   test_services - unit tests for the hardware-free services
#   test_logger   - the real logger, storage, SD driver and FatFs against a simulated SD card
CC     ?= cc
WARN   := -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
SVC    := ../App/services
FATFS  := ../Middlewares/Third_Party/FatFs/src

SVC_SRCS    := $(SVC)/ringbuf.c $(SVC)/logrec.c $(SVC)/jsonl.c $(SVC)/timekeeping.c
LOGGER_SRCS := test_logger.c ../App/app/logger.c $(SVC_SRCS) $(SVC)/storage.c ../App/drivers/sd_spi.c
VENDOR_SRCS := ../FATFS/Target/user_diskio.c ../FATFS/App/app_fatfs.c $(FATFS)/ff.c $(FATFS)/ff_gen_drv.c \
               $(FATFS)/diskio.c $(FATFS)/option/ccsbcs.c
FW_INC      := -Ifake -I../App/app -I../App/board -I../App/config -I../App/drivers -I$(SVC) \
               -I../Core/Inc -I../FATFS/App -I../FATFS/Target -I$(FATFS)

test: out/test_services out/test_logger
	./out/test_services
	./out/test_logger
	python3 check_logs.py

out/test_services: test_services.c $(SVC_SRCS) $(wildcard $(SVC)/*.h)
	@mkdir -p out
	$(CC) $(WARN) -I$(SVC) -I../App/drivers test_services.c $(SVC_SRCS) -o $@

# ST's and ChaN's code is built without our warning flags; ours with -Werror.
out/vendor.o: $(VENDOR_SRCS) $(wildcard fake/*.h ../FATFS/Target/*.h)
	@mkdir -p out/vendor
	cd out/vendor && $(CC) -std=gnu11 -w -g '-D__weak=__attribute__((weak))' $(addprefix -I../../,$(patsubst -I%,%,$(FW_INC))) -c $(addprefix ../../,$(VENDOR_SRCS))
	$(CC) -r -o $@ out/vendor/*.o

out/test_logger: $(LOGGER_SRCS) out/vendor.o $(wildcard ../App/*/*.h fake/*.h)
	$(CC) $(WARN) $(FW_INC) $(LOGGER_SRCS) out/vendor.o -o $@

clean:
	rm -rf out

.PHONY: test clean
