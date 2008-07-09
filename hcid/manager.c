/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>

#include <dbus/dbus.h>

#include <gdbus.h>

#include "logging.h"
#include "textfile.h"
#include "hcid.h"
#include "sdpd.h"
#include "adapter.h"
#include "dbus-common.h"
#include "error.h"
#include "dbus-hci.h"
#include "dbus-database.h"
#include "sdp-xml.h"
#include "oui.h"

#include "manager.h"

#define MAX_DEVICES		16

static DBusConnection *connection = NULL;
static int default_adapter_id = -1;
static GSList *adapters = NULL;

struct hci_peer {
	struct timeval lastseen;
	struct timeval lastused;

	bdaddr_t bdaddr;
	uint32_t class;
	int8_t   rssi;
	uint8_t  data[240];
	uint8_t  name[248];

	uint8_t  pscan_rep_mode;
	uint8_t  pscan_period_mode;
	uint8_t  pscan_mode;
	uint16_t clock_offset;

	struct hci_peer *next;
};

struct hci_conn {
	bdaddr_t bdaddr;
	uint16_t handle;

	struct hci_conn *next;
};

struct hci_dev {
	int ignore;

	bdaddr_t bdaddr;
	uint8_t  features[8];
	uint8_t  lmp_ver;
	uint16_t lmp_subver;
	uint16_t hci_rev;
	uint16_t manufacturer;

	uint8_t  ssp_mode;
	uint8_t  name[248];
	uint8_t  class[3];

	struct hci_peer *peers;
	struct hci_conn *conns;
};

static struct hci_dev devices[MAX_DEVICES];

#define ASSERT_DEV_ID { if (dev_id >= MAX_DEVICES) return -ERANGE; }

void init_adapters(void)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++)
		memset(devices + i, 0, sizeof(struct hci_dev));
}

static int device_read_bdaddr(uint16_t dev_id, bdaddr_t *bdaddr)
{
	int dd, err;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return -err;
	}

	if (hci_read_bd_addr(dd, bdaddr, 2000) < 0) {
		err = errno;
		error("Can't read address for hci%d: %s (%d)",
					dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	hci_close_dev(dd);

	return 0;
}

int add_adapter(uint16_t dev_id)
{
	struct hci_dev *dev;
	struct hci_dev_info di;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	if (hci_devinfo(dev_id, &di) < 0) {
		dev->ignore = 1;
		return -errno;
	}

	if (hci_test_bit(HCI_RAW, &di.flags)) {
		info("Device hci%d is using raw mode", dev_id);
		dev->ignore = 1;
	}

	if (bacmp(&di.bdaddr, BDADDR_ANY))
		bacpy(&dev->bdaddr, &di.bdaddr);
	else {
		int err = device_read_bdaddr(dev_id, &dev->bdaddr);
		if (err < 0)
			return err;
	}
	memcpy(dev->features, di.features, 8);

	info("Device hci%d has been added", dev_id);

	return 0;
}

int remove_adapter(uint16_t dev_id)
{
	struct hci_dev *dev;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	memset(dev, 0, sizeof(struct hci_dev));

	info("Device hci%d has been removed", dev_id);

	return 0;
}

static inline uint8_t get_inquiry_mode(struct hci_dev *dev)
{
	if (dev->features[6] & LMP_EXT_INQ)
		return 2;

	if (dev->features[3] & LMP_RSSI_INQ)
		return 1;

	if (dev->manufacturer == 11 &&
			dev->hci_rev == 0x00 && dev->lmp_subver == 0x0757)
		return 1;

	if (dev->manufacturer == 15) {
		if (dev->hci_rev == 0x03 && dev->lmp_subver == 0x6963)
			return 1;
		if (dev->hci_rev == 0x09 && dev->lmp_subver == 0x6963)
			return 1;
		if (dev->hci_rev == 0x00 && dev->lmp_subver == 0x6965)
			return 1;
	}

	if (dev->manufacturer == 31 &&
			dev->hci_rev == 0x2005 && dev->lmp_subver == 0x1805)
		return 1;

	return 0;
}

static void update_ext_inquiry_response(int dd, struct hci_dev *dev)
{
	uint8_t fec = 0, data[240];

	if (!(dev->features[6] & LMP_EXT_INQ))
		return;

	memset(data, 0, sizeof(data));

	if (dev->ssp_mode > 0)
		create_ext_inquiry_response((char *) dev->name, data);

	if (hci_write_ext_inquiry_response(dd, fec, data, 2000) < 0)
		error("Can't write extended inquiry response: %s (%d)",
						strerror(errno), errno);
}

int start_adapter(uint16_t dev_id)
{
	struct hci_dev *dev;
	struct hci_version ver;
	uint8_t features[8], inqmode;
	uint8_t events[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x00 };
	char name[249];
	int dd, err;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	if (dev->ignore)
		return 0;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return -err;
	}

	if (hci_read_local_version(dd, &ver, 1000) < 0) {
		err = errno;
		error("Can't read version info for hci%d: %s (%d)",
					dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	dev->hci_rev = ver.hci_rev;
	dev->lmp_ver = ver.lmp_ver;
	dev->lmp_subver = ver.lmp_subver;
	dev->manufacturer = ver.manufacturer;

	if (hci_read_local_features(dd, features, 1000) < 0) {
		err = errno;
		error("Can't read features for hci%d: %s (%d)",
					dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	memcpy(dev->features, features, 8);

	if (hci_read_class_of_dev(dd, dev->class, 1000) < 0) {
		err = errno;
		error("Can't read class of device on hci%d: %s (%d)",
						dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	if (hci_read_local_name(dd, sizeof(name), name, 2000) < 0) {
		err = errno;
		error("Can't read local name on hci%d: %s (%d)",
						dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	memcpy(dev->name, name, 248);

	if (!(features[6] & LMP_SIMPLE_PAIR))
		goto setup;

	if (hcid_dbus_use_experimental()) {
		if (ioctl(dd, HCIGETAUTHINFO, NULL) < 0 && errno != EINVAL)
			hci_write_simple_pairing_mode(dd, 0x01, 2000);
	}

	if (hci_read_simple_pairing_mode(dd, &dev->ssp_mode, 1000) < 0) {
		err = errno;
		error("Can't read simple pairing mode on hci%d: %s (%d)",
						dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

setup:
	if (ver.hci_rev > 1) {
		if (features[5] & LMP_SNIFF_SUBR)
			events[5] |= 0x20;

		if (features[5] & LMP_PAUSE_ENC)
			events[5] |= 0x80;

		if (features[6] & LMP_EXT_INQ)
			events[5] |= 0x40;

		if (features[6] & LMP_NFLUSH_PKTS)
			events[7] |= 0x01;

		if (features[7] & LMP_LSTO)
			events[6] |= 0x80;

		if (features[6] & LMP_SIMPLE_PAIR) {
			events[6] |= 0x01;	/* IO Capability Request */
			events[6] |= 0x02;	/* IO Capability Response */
			events[6] |= 0x04;	/* User Confirmation Request */
			events[6] |= 0x08;	/* User Passkey Request */
			events[6] |= 0x10;	/* Remote OOB Data Request */
			events[6] |= 0x20;	/* Simple Pairing Complete */
			events[7] |= 0x04;	/* User Passkey Notification */
			events[7] |= 0x08;	/* Keypress Notification */
			events[7] |= 0x10;	/* Remote Host Supported Features Notification */
		}

		hci_send_cmd(dd, OGF_HOST_CTL, OCF_SET_EVENT_MASK,
						sizeof(events), events);
	}

	if (read_local_name(&dev->bdaddr, name) == 0) {
		memcpy(dev->name, name, 248);
		hci_write_local_name(dd, name, 5000);
        }

	update_ext_inquiry_response(dd, dev);

	inqmode = get_inquiry_mode(dev);
	if (inqmode < 1)
		goto done;

	if (hci_write_inquiry_mode(dd, inqmode, 2000) < 0) {
		err = errno;
		error("Can't write inquiry mode for hci%d: %s (%d)",
						dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

done:
	hci_close_dev(dd);

	info("Device hci%d has been activated", dev_id);

	return 0;
}

int stop_adapter(uint16_t dev_id)
{
	ASSERT_DEV_ID;

	info("Device hci%d has been disabled", dev_id);

	return 0;
}

int update_adapter(uint16_t dev_id)
{
	struct hci_dev *dev;
	int dd;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	if (dev->ignore)
		return 0;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		int err = errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return -err;
	}

	update_ext_inquiry_response(dd, dev);

	hci_close_dev(dd);

	return 0;
}

int get_device_address(uint16_t dev_id, char *address, size_t size)
{
	struct hci_dev *dev;

	ASSERT_DEV_ID;

	if (size < 18)
		return -ENOBUFS;

	dev = &devices[dev_id];

	return ba2str(&dev->bdaddr, address);
}

int get_device_class(uint16_t dev_id, uint8_t *cls)
{
	struct hci_dev *dev;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];
	memcpy(cls, dev->class, 3);

	return 0;
}

int set_device_class(uint16_t dev_id, uint8_t *cls)
{
	struct hci_dev *dev;

	ASSERT_DEV_ID;
	dev = &devices[dev_id];
	memcpy(dev->class, cls, 3);

	return 0;
}

int get_device_version(uint16_t dev_id, char *version, size_t size)
{
	struct hci_dev *dev;
	char edr[7], *tmp;
	int err;

	ASSERT_DEV_ID;

	if (size < 14)
		return -ENOBUFS;

	dev = &devices[dev_id];

	if ((dev->lmp_ver == 0x03 || dev->lmp_ver == 0x04) &&
			(dev->features[3] & (LMP_EDR_ACL_2M | LMP_EDR_ACL_3M)))
		sprintf(edr, " + EDR");
	else
		edr[0] = '\0';

	tmp = lmp_vertostr(dev->lmp_ver);

	if (strlen(tmp) == 0)
		err = snprintf(version, size, "not assigned");
	else
		err = snprintf(version, size, "Bluetooth %s%s", tmp, edr);

	bt_free(tmp);

	return err;
}

static int digi_revision(uint16_t dev_id, char *revision, size_t size)
{
	struct hci_request rq;
	unsigned char req[] = { 0x07 };
	unsigned char buf[102];
	int dd, err;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return -err;
	}

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_VENDOR_CMD;
	rq.ocf    = 0x000e;
	rq.cparam = req;
	rq.clen   = sizeof(req);
	rq.rparam = &buf;
	rq.rlen   = sizeof(buf);

	if (hci_send_req(dd, &rq, 2000) < 0) {
		err = errno;
		error("Can't read revision for hci%d: %s (%d)",
					dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	hci_close_dev(dd);

	return snprintf(revision, size, "%s", buf + 1);
}

int get_device_revision(uint16_t dev_id, char *revision, size_t size)
{
	struct hci_dev *dev;
	int err;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	switch (dev->manufacturer) {
	case 10:
		err = snprintf(revision, size, "Build %d", dev->lmp_subver);
		break;
	case 12:
		err = digi_revision(dev_id, revision, size);
		break;
	case 15:
		err = snprintf(revision, size, "%d.%d / %d",
				dev->hci_rev & 0xff,
				dev->lmp_subver >> 8, dev->lmp_subver & 0xff);
		break;
	default:
		err = snprintf(revision, size, "0x%02x", dev->lmp_subver);
		break;
	}

	return err;
}

int get_device_manufacturer(uint16_t dev_id, char *manufacturer, size_t size)
{
	char *tmp;

	ASSERT_DEV_ID;

	tmp = bt_compidtostr(devices[dev_id].manufacturer);

	return snprintf(manufacturer, size, "%s", tmp);
}

int get_device_company(uint16_t dev_id, char *company, size_t size)
{
	char *tmp, oui[9];
	int err;

	ASSERT_DEV_ID;

	ba2oui(&devices[dev_id].bdaddr, oui);
	tmp = ouitocomp(oui);

	err = snprintf(company, size, "%s", tmp);

	free(tmp);

	return err;
}

int set_simple_pairing_mode(uint16_t dev_id, uint8_t mode)
{
	struct hci_dev *dev;
	int dd;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	dev->ssp_mode = mode;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		int err = errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return -err;
	}

	update_ext_inquiry_response(dd, dev);

	hci_close_dev(dd);

	return 0;
}

int get_device_name(uint16_t dev_id, char *name, size_t size)
{
	char tmp[249];
	int dd, err;

	ASSERT_DEV_ID;

	memset(tmp, 0, sizeof(tmp));

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return -err;
	}

	if (hci_read_local_name(dd, sizeof(tmp), tmp, 2000) < 0) {
		err = errno;
		error("Can't read name for hci%d: %s (%d)",
					dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	hci_close_dev(dd);

	memcpy(devices[dev_id].name, tmp, 248);

	return snprintf(name, size, "%s", tmp);
}

int set_device_name(uint16_t dev_id, const char *name)
{
	struct hci_dev *dev;
	int dd, err;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = errno;
		error("Can't open device hci%d: %s (%d)",
					dev_id, strerror(err), err);
		return -err;
	}

	if (hci_write_local_name(dd, name, 5000) < 0) {
		err = errno;
		error("Can't write name for hci%d: %s (%d)",
					dev_id, strerror(err), err);
		hci_close_dev(dd);
		return -err;
	}

	strncpy((char *) dev->name, name, 248);

	update_ext_inquiry_response(dd, dev);

	hci_close_dev(dd);

	return 0;
}

int get_device_alias(uint16_t dev_id, const bdaddr_t *bdaddr, char *alias, size_t size)
{
	char filename[PATH_MAX + 1], addr[18], *tmp;
	int err;

	ASSERT_DEV_ID;

	ba2str(&devices[dev_id].bdaddr, addr);
	create_name(filename, PATH_MAX, STORAGEDIR, addr, "aliases");

	ba2str(bdaddr, addr);

	tmp = textfile_get(filename, addr);
	if (!tmp)
		return -ENXIO;

	err = snprintf(alias, size, "%s", tmp);

	free(tmp);

	return err;
}

int set_device_alias(uint16_t dev_id, const bdaddr_t *bdaddr, const char *alias)
{
	char filename[PATH_MAX + 1], addr[18];

	ASSERT_DEV_ID;

	ba2str(&devices[dev_id].bdaddr, addr);
	create_name(filename, PATH_MAX, STORAGEDIR, addr, "aliases");

	create_file(filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	ba2str(bdaddr, addr);

	return textfile_put(filename, addr, alias);
}

int get_encryption_key_size(uint16_t dev_id, const bdaddr_t *baddr)
{
	struct hci_dev *dev;
	int size;

	ASSERT_DEV_ID;

	dev = &devices[dev_id];

	switch (dev->manufacturer) {
	default:
		size = -ENOENT;
		break;
	}

	return size;
}

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static inline DBusMessage *no_such_adapter(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".NoSuchAdapter",
			"No such adapter");
}

static inline DBusMessage *no_such_service(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".NoSuchService",
			"No such service");
}

static inline DBusMessage *failed_strerror(DBusMessage *msg, int err)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".Failed",
			strerror(err));
}

static int find_by_address(const char *str)
{
	struct hci_dev_list_req *dl;
	struct hci_dev_req *dr;
	bdaddr_t ba;
	int i, sk;
	int devid = -1;

	sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sk < 0)
		return -1;

	dl = g_malloc0(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl));

	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(sk, HCIGETDEVLIST, dl) < 0)
		goto out;

	dr = dl->dev_req;
	str2ba(str, &ba);

	for (i = 0; i < dl->dev_num; i++, dr++) {
		struct hci_dev_info di;

		if (hci_devinfo(dr->dev_id, &di) < 0)
			continue;

		if (hci_test_bit(HCI_RAW, &di.flags))
			continue;

		if (!bacmp(&ba, &di.bdaddr)) {
			devid = dr->dev_id;
			break;
		}
	}

out:
	g_free(dl);
	close(sk);
	return devid;
}

static DBusMessage *default_adapter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct adapter *adapter;

	adapter = manager_find_adapter_by_id(default_adapter_id);
	if (!adapter)
		return no_such_adapter(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &adapter->path,
				DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *find_adapter(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply;
	struct adapter *adapter;
	struct hci_dev_info di;
	const char *pattern;
	int dev_id;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID))
		return NULL;

	/* hci_devid() would make sense to use here, except it
	   is restricted to devices which are up */
	if (!strncmp(pattern, "hci", 3) && strlen(pattern) >= 4)
		dev_id = atoi(pattern + 3);
	else
		dev_id = find_by_address(pattern);

	if (dev_id < 0)
		return no_such_adapter(msg);

	if (hci_devinfo(dev_id, &di) < 0)
		return no_such_adapter(msg);

	if (hci_test_bit(HCI_RAW, &di.flags))
		return no_such_adapter(msg);

	adapter = manager_find_adapter_by_id(dev_id);
	if (!adapter)
		return no_such_adapter(msg);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &adapter->path,
				DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *list_adapters(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessage *reply;
	GSList *l;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (l = adapters; l; l = l->next) {
		struct adapter *adapter = l->data;
		struct hci_dev_info di;

		if (hci_devinfo(adapter->dev_id, &di) < 0)
			continue;

		if (hci_test_bit(HCI_RAW, &di.flags))
			continue;

		dbus_message_iter_append_basic(&array_iter,
					DBUS_TYPE_OBJECT_PATH, &adapter->path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return reply;
}

static GDBusMethodTable manager_methods[] = {
	{ "DefaultAdapter",	"",	"o",	default_adapter	},
	{ "FindAdapter",	"s",	"o",	find_adapter	},
	{ "ListAdapters",	"",	"ao",	list_adapters	},
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "AdapterAdded",		"o"	},
	{ "AdapterRemoved",		"o"	},
	{ "DefaultAdapterChanged",	"o"	},
	{ }
};

dbus_bool_t manager_init(DBusConnection *conn, const char *path)
{
	connection = conn;

	return g_dbus_register_interface(conn, "/", MANAGER_INTERFACE,
			manager_methods, manager_signals,
			NULL, NULL, NULL);
}

void manager_cleanup(DBusConnection *conn, const char *path)
{
	g_dbus_unregister_interface(conn, "/", MANAGER_INTERFACE);
}

static gint adapter_id_cmp(gconstpointer a, gconstpointer b)
{
	const struct adapter *adapter = a;
	uint16_t id = GPOINTER_TO_UINT(b);

	return adapter->dev_id == id ? 0 : -1;
}

static gint adapter_path_cmp(gconstpointer a, gconstpointer b)
{
	const struct adapter *adapter = a;
	const char *path = b;

	return strcmp(adapter->path, path);
}

static gint adapter_address_cmp(gconstpointer a, gconstpointer b)
{
	const struct adapter *adapter = a;
	const char *address = b;

	return strcmp(adapter->address, address);
}

struct adapter *manager_find_adapter(const bdaddr_t *sba)
{
	GSList *match;
	char address[18];

	ba2str(sba, address);

	match = g_slist_find_custom(adapters, address, adapter_address_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct adapter *manager_find_adapter_by_path(const char *path)
{
	GSList *match;

	match = g_slist_find_custom(adapters, path, adapter_path_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct adapter *manager_find_adapter_by_id(int id)
{
	GSList *match;

	match = g_slist_find_custom(adapters, GINT_TO_POINTER(id), adapter_id_cmp);
	if (!match)
		return NULL;

	return match->data;
}

void manager_add_adapter(struct adapter *adapter)
{
	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE, "AdapterAdded",
			DBUS_TYPE_OBJECT_PATH, &adapter->path,
			DBUS_TYPE_INVALID);

	adapters = g_slist_append(adapters, adapter);
}

void manager_remove_adapter(struct adapter *adapter)
{
	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE, "AdapterRemoved",
			DBUS_TYPE_OBJECT_PATH, &adapter->path,
			DBUS_TYPE_INVALID);

	if ((default_adapter_id == adapter->dev_id || default_adapter_id < 0)) {
		int new_default = hci_get_route(NULL);

		if (new_default >= 0)
			manager_set_default_adapter(new_default);
	}

	adapters = g_slist_remove(adapters, adapter);
}

int manager_get_default_adapter()
{
	return default_adapter_id;
}

void manager_set_default_adapter(int id)
{
	struct adapter *adapter = manager_find_adapter_by_id(id);

	default_adapter_id = id;

	g_dbus_emit_signal(connection, "/",
			MANAGER_INTERFACE,
			"DefaultAdapterChanged",
			DBUS_TYPE_OBJECT_PATH, &adapter->path,
			DBUS_TYPE_INVALID);
}
