/*
 * Copyright © 2011 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *	Peter Hutterer (peter.hutterer@redhat.com)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "libwacomint.h"

#include <glib.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SUFFIX ".tablet"
#define FEATURE_GROUP "Features"
#define DEVICE_GROUP "Device"

static WacomClass
libwacom_model_string_to_enum(const char *model)
{
	if (model == NULL || *model == '\0')
		return WCLASS_UNKNOWN;

	if (strcmp(model, "Intuos3") == 0)
		return WCLASS_INTUOS3;
	if (strcmp(model, "Intuos4") == 0)
		return WCLASS_INTUOS4;
	if (strcmp(model, "Cintiq") == 0)
		return WCLASS_CINTIQ;
	if (strcmp(model, "Bamboo") == 0)
		return WCLASS_BAMBOO;
	if (strcmp(model, "Graphire") == 0)
		return WCLASS_GRAPHIRE;

	return WCLASS_UNKNOWN;
}

WacomStylusType
type_from_str (const char *type)
{
	if (type == NULL)
		return WSTYLUS_UNKNOWN;
	if (strcmp (type, "General") == 0)
		return WSTYLUS_GENERAL;
	if (strcmp (type, "Inking") == 0)
		return WSTYLUS_INKING;
	if (strcmp (type, "Airbrush") == 0)
		return WSTYLUS_AIRBRUSH;
	if (strcmp (type, "Classic") == 0)
		return WSTYLUS_CLASSIC;
	if (strcmp (type, "Marker") == 0)
		return WSTYLUS_MARKER;
	return WSTYLUS_UNKNOWN;
}

WacomBusType
bus_from_str (const char *str)
{
	if (strcmp (str, "usb") == 0)
		return WBUSTYPE_USB;
	if (strcmp (str, "serial") == 0)
		return WBUSTYPE_SERIAL;
	if (strcmp (str, "bluetooth") == 0)
		return WBUSTYPE_BLUETOOTH;
	return WBUSTYPE_UNKNOWN;
}

const char *
bus_to_str (WacomBusType bus)
{
	switch (bus) {
	case WBUSTYPE_UNKNOWN:
		g_assert_not_reached();
		break;
	case WBUSTYPE_USB:
		return "usb";
	case WBUSTYPE_SERIAL:
		return "bluetooth";
	case WBUSTYPE_BLUETOOTH:
		return "bluetooth";
	}
	g_assert_not_reached ();
}

static int
libwacom_matchstr_to_ints(const char *match, uint32_t *vendor_id, uint32_t *product_id, WacomBusType *bus)
{
	char busstr[64];
	int rc;

	rc = sscanf(match, "%63[^:]:%x:%x", busstr, vendor_id, product_id);
	if (rc != 3)
		return 0;

	*bus = bus_from_str (busstr);

	return 1;
}

static void
libwacom_parse_stylus_keyfile(WacomDeviceDatabase *db, const char *path)
{
	GKeyFile *keyfile;
	GError *error = NULL;
	char **groups;
	gboolean rc;
	guint i;

	keyfile = g_key_file_new();

	rc = g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &error);
	g_assert (rc);

	groups = g_key_file_get_groups (keyfile, NULL);
	for (i = 0; groups[i]; i++) {
		WacomStylus *stylus;
		GError *error = NULL;
		char *type;
		int id;

		id = strtol (groups[i], NULL, 16);
		if (id == 0) {
			g_warning ("Failed to parse stylus ID '%s'", groups[i]);
			continue;
		}

		stylus = g_new0 (WacomStylus, 1);
		stylus->id = id;
		stylus->name = g_key_file_get_string(keyfile, groups[i], "Name", NULL);

		stylus->is_eraser = g_key_file_get_boolean(keyfile, groups[i], "IsEraser", NULL);

		if (stylus->is_eraser == FALSE) {
			stylus->has_eraser = g_key_file_get_boolean(keyfile, groups[i], "HasEraser", NULL);
			stylus->num_buttons = g_key_file_get_integer(keyfile, groups[i], "Buttons", &error);
			if (stylus->num_buttons == 0 && error != NULL) {
				stylus->num_buttons = -1;
				g_clear_error (&error);
			}
		} else {
			stylus->num_buttons = 0;
			stylus->has_eraser = FALSE;
		}

		type = g_key_file_get_string(keyfile, groups[i], "Type", NULL);
		stylus->type = type_from_str (type);
		g_free (type);

		if (g_hash_table_lookup (db->stylus_ht, GINT_TO_POINTER (id)) != NULL)
			g_warning ("Duplicate definition for stylus ID '0x%x'", id);

		g_hash_table_insert (db->stylus_ht, GINT_TO_POINTER (id), stylus);
	}
	g_strfreev (groups);
}

static WacomDevice*
libwacom_parse_tablet_keyfile(const char *path)
{
	WacomDevice *device = NULL;
	GKeyFile *keyfile;
	GError *error = NULL;
	gboolean rc;
	char *class;
	char *match;
	char **styli_list;

	keyfile = g_key_file_new();

	rc = g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, &error);

	if (!rc) {
		DBG("%s: %s\n", path, error->message);
		goto out;
	}

	device = g_new0 (WacomDevice, 1);

	device->vendor = g_key_file_get_string(keyfile, DEVICE_GROUP, "Vendor", NULL);
	device->product = g_key_file_get_string(keyfile, DEVICE_GROUP, "Product", NULL);
	device->width = g_key_file_get_integer(keyfile, DEVICE_GROUP, "Width", NULL);
	device->height = g_key_file_get_integer(keyfile, DEVICE_GROUP, "Height", NULL);

	class = g_key_file_get_string(keyfile, DEVICE_GROUP, "Class", NULL);
	device->cls = libwacom_model_string_to_enum(class);
	g_free(class);

	match = g_key_file_get_string(keyfile, DEVICE_GROUP, "DeviceMatch", NULL);
	if (g_strcmp0 (match, GENERIC_DEVICE_MATCH) == 0) {
		device->match = match;
	} else {
		if (!libwacom_matchstr_to_ints(match, &device->vendor_id, &device->product_id, &device->bus))
			DBG("failed to match '%s' for product/vendor IDs in '%s'\n", device->match, path);
		else
			device->match = g_strdup_printf ("%s:0x%x:0x%x", bus_to_str (device->bus), device->vendor_id, device->product_id);
		g_free (match);
	}

	styli_list = g_key_file_get_string_list(keyfile, DEVICE_GROUP, "Styli", NULL, NULL);
	if (styli_list) {
		GArray *array;
		guint i;

		array = g_array_new (FALSE, FALSE, sizeof(int));
		device->num_styli = 0;
		for (i = 0; styli_list[i]; i++) {
			glong long_value = strtol (styli_list[i], NULL, 0);
			int int_value = long_value;

			g_array_append_val (array, int_value);
			device->num_styli++;
		}
		g_strfreev (styli_list);
		device->supported_styli = (int *) g_array_free (array, FALSE);
	}

	/* Features */
	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "Stylus", NULL))
		device->features |= FEATURE_STYLUS;

	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "Touch", NULL))
		device->features |= FEATURE_TOUCH;

	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "Ring", NULL))
		device->features |= FEATURE_RING;

	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "Ring2", NULL))
		device->features |= FEATURE_RING2;

	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "VStrip", NULL))
		device->features |= FEATURE_VSTRIP;

	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "HStrip", NULL))
		device->features |= FEATURE_HSTRIP;

	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "BuiltIn", NULL))
		device->features |= FEATURE_BUILTIN;

	if (g_key_file_get_boolean(keyfile, FEATURE_GROUP, "Reversible", NULL))
		device->features |= FEATURE_REVERSIBLE;

	device->num_buttons = g_key_file_get_integer(keyfile, FEATURE_GROUP, "Buttons", NULL);

out:
	if (keyfile)
		g_key_file_free(keyfile);
	if (error)
		g_error_free(error);

	return device;
}

static int
scandir_filter(const struct dirent *entry)
{
	const char *name = entry->d_name;
	int len, suffix_len;

	if (!name || name[0] == '.')
		return 0;

	len = strlen(name);
	suffix_len = strlen(SUFFIX);
	if (len <= suffix_len)
		return 0;

	return !strcmp(&name[len - suffix_len], SUFFIX);
}

WacomDeviceDatabase *
libwacom_database_new (void)
{
    int n, nfiles;
    struct dirent **files;
    WacomDeviceDatabase *db;
    char *path;

    db = g_new0 (WacomDeviceDatabase, 1);

    /* Load tablets */
    db->device_ht = g_hash_table_new_full (g_str_hash,
					   g_str_equal,
					   g_free,
					   (GDestroyNotify) libwacom_destroy);

    n = scandir(DATADIR, &files, scandir_filter, alphasort);
    if (n <= 0)
	    return db;

    nfiles = n;
    while(n--) {
	    WacomDevice *d;

	    path = g_build_filename (DATADIR, files[n]->d_name, NULL);
	    d = libwacom_parse_tablet_keyfile(path);
	    g_free(path);

	    if (d)
		    g_hash_table_insert (db->device_ht, g_strdup (d->match), d);
    }

    while(nfiles--)
	    free(files[nfiles]);
    free(files);

    /* Load styli */
    path = g_build_filename (DATADIR, STYLUS_DATA_FILE, NULL);
    db->stylus_ht = g_hash_table_new_full (g_direct_hash,
					   g_direct_equal,
					   NULL,
					   (GDestroyNotify) libwacom_stylus_destroy);
    libwacom_parse_stylus_keyfile(db, path);

    return db;
}

void
libwacom_database_destroy(WacomDeviceDatabase *db)
{
	g_hash_table_destroy(db->device_ht);
	g_hash_table_destroy(db->stylus_ht);
	g_free (db);
}

/* vim: set noexpandtab tabstop=8 shiftwidth=8: */