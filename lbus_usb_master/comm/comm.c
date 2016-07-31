#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libusb.h>

#include "../../lbus_common/lbus_data.h"

static libusb_context *ctx = NULL;
static libusb_device_handle *dev;

#define error(params) { \
	fprintf(stderr, params); \
	exit(-1); \
	}
#define cl1error(params) { \
	libusb_exit(ctx); \
	error(params); \
	}
#define clerror(params) { \
	libusb_close(dev); \
	cl1error(params); \
	}
#define rlerror(params) { \
	libusb_release_interface(dev, 0); \
	clerror(params); \
	}

#define MAX_FW_SIZE (44*1024)
#define FW_START_PAGE 4

#define LIBUSB_TIMEOUT 1000
#define LIBUSB_TXTIMEOUT 20

static int tx(void* buf, int size) {
	int done = 0;
	int transferred = 0;
	uint8_t tbuf[64];
	while(done < size) {
		const int chunk_size = size > 63 ? 63 : size;
		tbuf[0] = 1; // XMIT
		memcpy(tbuf+1, buf+done, chunk_size);
		int res = libusb_bulk_transfer(dev, 0x01, tbuf, chunk_size+1, &transferred, LIBUSB_TXTIMEOUT);
		if(res == 0 || res == LIBUSB_ERROR_TIMEOUT) {
			done += transferred;
		} else {
			error("error during communication (transmit)\n");
		}
	}
	return done;
}

static int rx(void* buf, int size) {
	int done = 0;
	int transferred = 0;
	uint8_t tbuf[2] = { 2 /* RECV */, 0 };
	while(done < size) {
		tbuf[1] = size > 64 ? 64 : size;
		int res = libusb_bulk_transfer(dev, 0x01, tbuf, 2, &transferred, LIBUSB_TXTIMEOUT);
		if(res != 0) {
			error("error during communication (recv cmd)\n");
		}
		transferred = 0;
		res = libusb_bulk_transfer(dev, 0x82, buf + done, tbuf[1], &transferred, LIBUSB_TIMEOUT);
		done += transferred;
		if(res == LIBUSB_ERROR_TIMEOUT || res == 0) {
			if(transferred < tbuf[1])
				break;
		} else {
			error("error during communication (recv)\n");
		}
	}
	return done;
}

static void echo() {
	uint8_t tbuf[64] = { 3 /* ECHO */, 1,2,3,4,5,6,7,8,9,10 };
	uint8_t rbuf[63];
	int transferred = 0;
	for(int i=0; i<16384; i++) {
		transferred = 0;
		int res = libusb_bulk_transfer(dev, 0x01, tbuf, 64, &transferred, LIBUSB_TXTIMEOUT);
		if(res != 0 || transferred != 64) {
			error("error during communication (echo req)\n");
		}
		transferred = 0;
		res = libusb_bulk_transfer(dev, 0x82, rbuf, 63, &transferred, LIBUSB_TIMEOUT);
		if(res != 0 || transferred != 63) {
			error("error during communication (echo reply)\n");
		}
	}
}

static uint32_t crc32(uint32_t Crc, uint32_t Size, void *Buffer) {
	Size = Size >> 2; // /4

	while(Size--) {
		static const uint32_t CrcTable[16] = { // Nibble lookup table for 0x04C11DB7 polynomial
		0x00000000,0x04C11DB7,0x09823B6E,0x0D4326D9,0x130476DC,0x17C56B6B,0x1A864DB2,0x1E475005,
		0x2608EDB8,0x22C9F00F,0x2F8AD6D6,0x2B4BCB61,0x350C9B64,0x31CD86D3,0x3C8EA00A,0x384FBDBD };

		Crc = Crc ^ *((uint32_t *)Buffer); // Apply all 32-bits

		Buffer += 4;

		// Process 32-bits, 4 at a time, or 8 rounds

		Crc = (Crc << 4) ^ CrcTable[Crc >> 28]; // Assumes 32-bit reg, masking index to 4-bits
		Crc = (Crc << 4) ^ CrcTable[Crc >> 28]; //  0x04C11DB7 Polynomial used in STM32
		Crc = (Crc << 4) ^ CrcTable[Crc >> 28];
		Crc = (Crc << 4) ^ CrcTable[Crc >> 28];
		Crc = (Crc << 4) ^ CrcTable[Crc >> 28];
		Crc = (Crc << 4) ^ CrcTable[Crc >> 28];
		Crc = (Crc << 4) ^ CrcTable[Crc >> 28];
		Crc = (Crc << 4) ^ CrcTable[Crc >> 28];
	}

	return(Crc);
}

static void usage(void) {
	fprintf(stderr, "Usage: comm <dest_addr> <command> [parameters...]\n");
	exit(-1);
}

int readall(int fd, void *buf, size_t count) {
	int res = 0;
	int c = 0;
	while(c < count) {
		int res = read(fd, buf, count - c);
		if(res == -1) return -1;
		if(res == 0) return c;
		c += res;
		buf += res;
	}
	return c;
}

int main(int argc, char* argv[]) {
	if(libusb_init(&ctx)) {
		error("cannot initialize libusb.\n");
	}
	if(NULL == (dev = libusb_open_device_with_vid_pid(ctx, 0xdead, 0xcafe))) {
		cl1error("lbus master device not found.\n");
	}
	if(libusb_claim_interface(dev, 0)) {
		clerror("cannot claim interface.\n");
	}

	int ret = -1;
	uint8_t buf[4096];
	int optind = 1;
	if(argc == 2 && !strcasecmp("echo", argv[1])) {
		echo();
		goto success;
	}
	if(argc < 3) {
		usage();
	}
	int dst = strtol(argv[optind++], NULL, 0);
	if(dst < 0 || dst > 0xFF) usage();
	char *cmd = argv[optind++];
	if(!strcasecmp("ping", cmd)) {
		uint8_t pkg[] = { 5, 0, dst, PING };
		tx(pkg, sizeof(pkg));
		uint8_t reply;
		if(rx(&reply, 1) == 1) {
			fprintf(stderr, "got reply\n");
			goto success;
		} else {
			fprintf(stderr, "miss\n");
		}
	} else if(!strcasecmp("reset_to_bootloader", cmd)) {
		uint8_t pkg[] = { 4, 0, dst, RESET_TO_BOOTLOADER };
		tx(pkg, sizeof(pkg));
		goto success;
	} else if(!strcasecmp("reset_to_firmware", cmd)) {
		uint8_t pkg[] = { 4, 0, dst, RESET_TO_FIRMWARE };
		tx(pkg, sizeof(pkg));
		goto success;
	} else if(!strcasecmp("erase_config", cmd)) {
		uint8_t pkg[] = { 4, 0, dst, ERASE_CONFIG };
		tx(pkg, sizeof(pkg));
		goto success;
	} else if(!strcasecmp("get_data", cmd)) {
		if(optind + 1 >= argc) {
			fprintf(stderr, "usage: ... %s <type_id> <reply_size>\n", cmd);
			goto done;
		}
		int item = strtol(argv[optind++], NULL, 0);
		if(item < 1 || item > 0xFFFF) {
			fprintf(stderr, "bad type_id.\n");
			goto done;
		}
		int reply_size = strtol(argv[optind++], NULL, 0);
		if(reply_size < 0 || reply_size > sizeof(buf)) {
			fprintf(stderr, "bad reply_size.\n");
			goto done;
		}
		uint16_t d16 = 6 + reply_size;
		tx(&d16, 2);
		uint8_t d8[] = { dst, GET_DATA };
		tx(d8, 2);
		d16 = item;
		tx(&d16, 2);
		if(rx(&buf, reply_size) == reply_size) {
			fprintf(stderr, "got answer:\n");
			write(1, buf, reply_size);
			goto success;
		} else {
			fprintf(stderr, "error or no reply\n");
		}
	} else if(!strcasecmp("set_address", cmd)) {
		if(optind >= argc) {
			fprintf(stderr, "usage: ... %s <address>\n", cmd);
			goto done;
		}
		int address = strtol(argv[optind++], NULL, 0);
		if(address < 1 || address > 254) {
			fprintf(stderr, "bad address given.\n");
			goto done;
		}
		uint8_t pkg[] = { 6, 0, dst, SET_ADDRESS, address };
		tx(pkg, sizeof(pkg));
		uint8_t reply;
		if(rx(&reply, 1) == 1 && reply == 0) {
			fprintf(stderr, "success\n");
			goto success;
		} else {
			fprintf(stderr, "failure\n");
		}
	} else if(!strcasecmp("read_memory", cmd)) {
		if(optind + 1 >= argc) {
			fprintf(stderr, "usage: ... %s <address> <length>\n", cmd);
			goto done;
		}
		uint32_t address = strtoul(argv[optind++], NULL, 0);
		int length = (strtol(argv[optind++], NULL, 0)+3)&(~(3));
		if(length < 0 || length > sizeof(buf)) {
			fprintf(stderr, "bad reply_size.\n");
			goto done;
		}
		uint16_t d16 = 8 + length + 4;
		tx(&d16, 2);
		uint8_t d8[] = { dst, READ_MEMORY };
		tx(d8, 2);
		tx(&address, 4);
		uint32_t crc_test = 0;
		if(rx(buf, length) == length && rx(&crc_test, 4) == 4) {
			fprintf(stderr, "got answer (crc is %08X - ", crc_test);
			if(crc_test == crc32(0xFFFFFFFF, length, buf)) {
				fprintf(stderr, "OK)\n");
				write(1, buf, length);
				ret = 0;
			} else {
				fprintf(stderr, "WRONG)\n");
			}
		} else {
			fprintf(stderr, "error or no reply\n");
		}
	} else if(!strcasecmp("flash_firmware", cmd)) {
		if(optind >= argc) {
			fprintf(stderr, "usage: ... %s <firmware.bin>\n", cmd);
			goto done;
		}
		struct stat st;
		int ffd = open(argv[optind], O_RDONLY);
		if(ffd == -1) {
			perror("open firmware file");
			goto done;
		}
		if(fstat(ffd, &st) == -1) {
			close(ffd);
			perror("stat() firmware file");
			goto done;
		}
		if(st.st_size > MAX_FW_SIZE) {
			close(ffd);
			fprintf(stderr, "firmware file too large.\n");
			goto done;
		}
		int fw_pg_size = (st.st_size+(PAGE_SIZE-1)) & (~(PAGE_SIZE-1));
		void *fwbuf = calloc(1, fw_pg_size);
		int res = readall(ffd, fwbuf, st.st_size);
		if(res != st.st_size) {
			close(ffd);
			fprintf(stderr, "error reading firmware file.\n");
			goto done;
		}
		*((uint32_t*)(fwbuf + 0x150 + 4)) = fw_pg_size;
		*((uint32_t*)(fwbuf + 0x150 + 8)) = crc32(0xFFFFFFFF, fw_pg_size, fwbuf);
		void *p = fwbuf;
		uint16_t pg = FW_START_PAGE;
		for(void *p = fwbuf; p < (fwbuf+fw_pg_size); p+=PAGE_SIZE) {
			uint16_t d16 = 6 + PAGE_SIZE + 4 + 1;
			tx(&d16, 2);
			uint8_t d8[] = { dst, FLASH_FIRMWARE };
			tx(d8, 2);
			tx(&pg, 2);
			tx(p, PAGE_SIZE);
			uint32_t crc = crc32(0xFFFFFFFF, PAGE_SIZE, p);
			tx(&crc, 4);
			uint8_t reply;
			if(rx(&reply, 1) == 1 && reply == 0) {
				fprintf(stderr, "<%02X>", pg);
			} else {
				close(ffd);
				fprintf(stderr, " failure!\n");
				goto done;
			}
			pg++;
		}
		fprintf(stderr, " done!\n");
		close(ffd);
		goto success;
	} else {
		fprintf(stderr, "unknown command given.\n");
	}

success:
	ret = 0;

done:
	libusb_release_interface(dev, 0);
	libusb_close(dev);
	libusb_exit(ctx);
	return ret;
}