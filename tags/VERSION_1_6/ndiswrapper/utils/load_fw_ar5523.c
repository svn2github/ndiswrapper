/*
 *  Copyright (C) 2005 Laurent Goujon, Giridhar Pemmasani
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <usb.h>
#include <netinet/in.h>

/* ar5523 has 3 endpoints: 0x01, 0x02 and 0x81 */
#define EP1		0x01
#define EP2		0x02
#define EP3		0x81

#define BUFFER_SIZE	0x0800
#define WRITE_CMD	0x10
#define BULK_TIMEOUT	5000

#define ERROR(fmt, ...)							\
	fprintf(stderr, "%s(%d): " fmt "\n",				\
		__FUNCTION__, __LINE__ , ## __VA_ARGS__)

#define INFO(fmt, ...) fprintf(stdout, fmt "\n" , ## __VA_ARGS__)

struct {
	int vendor_id;
	int product_id;
} devices[] = {
	/* D-Link DWL-G132 */
	{0x2001, 0x3a01},
	{0x2001, 0x3a03},
	/* Netgear WG111U */
	{0x0cde, 0x0013},
	{0x0846, 0x4301},
	/* Netgear WG111T */
	{0x1385, 0x4251},
	/* Netgear WPN11 */
	{0x1385, 0x5f01},
	/* Trendnet TEW-444UB/504UB */
	{0x157e, 0x3206},
	{0x157e, 0x3007},
	/* end */
	{-1, -1},
};

/* these structures should be 512 bytes */
struct write_cmd {
	uint32_t code;
	uint32_t size;
	uint32_t total_size;
	uint32_t remaining_size;
	char padding[496];
};

struct read_cmd {
	/* code seems to be either 0x14 or 0x20 */
	uint32_t code;
	uint32_t size;
	uint32_t total_size;
	uint32_t remaining_size;
	uint32_t total_size2;
	char padding[492];
};

static int load_fw_ar5523(char *filename, usb_dev_handle *handle)
{
	int remaining_size, res, fd;
	struct write_cmd write_cmd;
	struct read_cmd read_cmd;
	struct stat fw_stat;
	ssize_t read_size;
	char *buffer;

	buffer = calloc(1, BUFFER_SIZE);
	if (!buffer) {
		ERROR("coudln't allocate memory");
		return -ENOMEM;
	}
	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		ERROR("couldn't open firmware file: %s", strerror(errno));
		goto err;
	}
	if (fstat(fd, &fw_stat) == -1) {
		ERROR("couldn't stat firmware file: %s", strerror(errno));
		goto err;
	}

	memset(&write_cmd, 0, sizeof(write_cmd));
	memset(&read_cmd, 0, sizeof(read_cmd));

	write_cmd.code = htonl(WRITE_CMD);
	remaining_size = fw_stat.st_size;
	write_cmd.total_size = htonl(remaining_size);

	while ((read_size = read(fd, buffer, BUFFER_SIZE)) > 0) {
		remaining_size -= read_size;
		write_cmd.size = htonl(read_size);
		write_cmd.remaining_size = htonl(remaining_size);

		res = usb_bulk_write(handle, EP1, (char *)&write_cmd,
				     sizeof(write_cmd), BULK_TIMEOUT);
		if (res < 0) {
			ERROR("couldn't write data: %s", usb_strerror());
			goto err;
		}
		res = usb_bulk_write(handle, EP2, buffer, read_size,
				     BULK_TIMEOUT);
		if (res < 0) {
			ERROR("couldn't write data: %s", usb_strerror());
			goto err;
		}
		res = usb_bulk_read(handle, EP3, (char *)&read_cmd,
				    sizeof(read_cmd), BULK_TIMEOUT);
		if (res < 0 || read_cmd.size != write_cmd.size ||
		    read_cmd.total_size != write_cmd.total_size ||
		    read_cmd.remaining_size != write_cmd.remaining_size) {
			ERROR("couldn't read data: %s", usb_strerror());
			goto err;
		}
	}
	if (remaining_size > 0) {
		ERROR("couldn't write all data - %d bytes left",
		      remaining_size);
		goto err;
	}
	free(buffer);
	close(fd);
	return 0;
err:
	free(buffer);
	if (fd > 0)
		close(fd);
	return -EINVAL;
}

int main(int argc, char *argv[])
{
	struct usb_bus *busses, *bus;
	int max_devnum;
	char *fw_file, *base_name;
	usb_dev_handle *handle;
	struct usb_device *dev;
	int res;
	
	if (argc != 2 && argc != 4) {
		ERROR("usage: %s <firmware file> [<vendor ID> <product ID>]",
		      argv[0]);
		return -1;
	}
	fw_file = argv[1];
	base_name = strrchr(fw_file, '/');
	if (base_name)
		base_name++;
	else
		base_name = fw_file;
	if (strcmp(base_name, "ar5523.bin")) {
		ERROR("file %s may not be valid firmware file; "
		      "file name should end with \"ar5523.bin\"", fw_file);
		return -2;
	}
	max_devnum = sizeof(devices) / sizeof(devices[0]);
	if (argc == 4) {
		devices[max_devnum - 1].vendor_id = strtol(argv[2], NULL, 16);
		devices[max_devnum - 1].product_id = strtol(argv[3], NULL, 16);
	}

	usb_init();
	usb_find_busses();
	usb_find_devices();

	busses = usb_get_busses();
	for (bus = busses; bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next) {
			int j;
			for (j = 0; j < max_devnum; j++) {
				if (dev->descriptor.idVendor ==
				    devices[j].vendor_id &&
				    dev->descriptor.idProduct ==
				    devices[j].product_id)
					goto found;
			}
		}
	}
	ERROR("no valid device found; if you are sure you have Atheros USB"
	      "based device, UNPLUG AND REPLUG THE DEVICE, run '%s' again "
	      "with vendor and product ids, which can be obtained with "
	      "'lsusb' command", argv[0]);
	return -3;

found:
	handle = usb_open(dev);
	if (!handle) {
		ERROR("couldn't open usb device");
		return -4;
	}
	if ((res = usb_set_configuration(handle, 1)) ||
	    (res = usb_claim_interface(handle, 0)))
		ERROR("couldn't set configuration: %s", usb_strerror());
	else {
		INFO("loading firmware for device 0x%04X:0x%04X ... ",
		     dev->descriptor.idVendor, dev->descriptor.idProduct);

		if ((res = load_fw_ar5523(fw_file, handle)) == 0)
			INFO("done");
		else
			INFO("failed");
		usb_release_interface(handle, 0);
	}

	usb_close(handle);
	if (res)
		return -5;
	else
		return 0;
}
