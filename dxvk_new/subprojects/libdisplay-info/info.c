#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "edid.h"
#include "info.h"
#include "log.h"
#include "memory-stream.h"

/* Generated file pnp-id-table.c: */
const char *
pnp_id_table(const char *key);

struct di_info *
di_info_parse_edid(const void *data, size_t size)
{
	struct di_edid *edid;
	struct di_info *info;
	struct memory_stream failure_msg;

	if (!memory_stream_open(&failure_msg))
		return NULL;

	edid = _di_edid_parse(data, size, failure_msg.fp);
	if (!edid)
		goto err_failure_msg_file;

	info = calloc(1, sizeof(*info));
	if (!info)
		goto err_edid;

	info->edid = edid;

	if (fflush(failure_msg.fp) != 0)
		goto err_info;

	info->failure_msg = memory_stream_close(&failure_msg);

	return info;

err_info:
	free(info);
err_edid:
	_di_edid_destroy(edid);
err_failure_msg_file:
	memory_stream_close(&failure_msg);
	return NULL;
}

void
di_info_destroy(struct di_info *info)
{
	_di_edid_destroy(info->edid);
	free(info->failure_msg);
	free(info);
}

const struct di_edid *
di_info_get_edid(const struct di_info *info)
{
	return info->edid;
}

const char *
di_info_get_failure_msg(const struct di_info *info)
{
	return info->failure_msg;
}

static void
encode_ascii_byte(FILE *out, char ch)
{
	uint8_t c = (uint8_t)ch;

	/*
	 * Replace ASCII control codes and non-7-bit codes
	 * with an escape string. The result is guaranteed to be valid
	 * UTF-8.
	 */
	if (c < 0x20 || c >= 0x7f)
		fprintf(out, "\\x%02x", c);
	else
		fputc(c, out);
}

static void
encode_ascii_string(FILE *out, const char *str)
{
	size_t len = strlen(str);
	size_t i;

	for (i = 0; i < len; i++)
		encode_ascii_byte(out, str[i]);
}

char *
di_info_get_make(const struct di_info *info)
{
	const struct di_edid_vendor_product *evp;
	char pnp_id[(sizeof(evp->manufacturer)) + 1] = { 0, };
	const char *manuf;
	struct memory_stream m;

	if (!info->edid)
		return NULL;

	if (!memory_stream_open(&m))
		return NULL;

	evp = di_edid_get_vendor_product(info->edid);
	memcpy(pnp_id, evp->manufacturer, sizeof(evp->manufacturer));

	manuf = pnp_id_table(pnp_id);
	if (manuf) {
		encode_ascii_string(m.fp, manuf);
		return memory_stream_close(&m);
	}

	fputs("PNP(", m.fp);
	encode_ascii_string(m.fp, pnp_id);
	fputs(")", m.fp);

	return memory_stream_close(&m);
}

char *
di_info_get_model(const struct di_info *info)
{
	const struct di_edid_vendor_product *evp;
	const struct di_edid_display_descriptor *const *desc;
	struct memory_stream m;
	size_t i;

	if (!info->edid)
		return NULL;

	if (!memory_stream_open(&m))
		return NULL;

	desc = di_edid_get_display_descriptors(info->edid);
	for (i = 0; desc[i]; i++) {
		if (di_edid_display_descriptor_get_tag(desc[i]) == DI_EDID_DISPLAY_DESCRIPTOR_PRODUCT_NAME) {
			encode_ascii_string(m.fp, di_edid_display_descriptor_get_string(desc[i]));
			return memory_stream_close(&m);
		}
	}

	evp = di_edid_get_vendor_product(info->edid);
	fprintf(m.fp, "0x%04" PRIX16, evp->product);

	return memory_stream_close(&m);
}

char *
di_info_get_serial(const struct di_info *info)
{
	const struct di_edid_display_descriptor *const *desc;
	const struct di_edid_vendor_product *evp;
	struct memory_stream m;
	size_t i;

	if (!info->edid)
		return NULL;

	if (!memory_stream_open(&m))
		return NULL;

	desc = di_edid_get_display_descriptors(info->edid);
	for (i = 0; desc[i]; i++) {
		if (di_edid_display_descriptor_get_tag(desc[i]) == DI_EDID_DISPLAY_DESCRIPTOR_PRODUCT_SERIAL) {
			encode_ascii_string(m.fp, di_edid_display_descriptor_get_string(desc[i]));
			return memory_stream_close(&m);
		}
	}

	evp = di_edid_get_vendor_product(info->edid);
	if (evp->serial != 0) {
		fprintf(m.fp, "0x%08" PRIX32, evp->serial);
		return memory_stream_close(&m);
	}

	free(memory_stream_close(&m));
	return NULL;
}
