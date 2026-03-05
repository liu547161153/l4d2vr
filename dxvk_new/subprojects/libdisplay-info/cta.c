#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "cta.h"
#include "log.h"
#include "edid.h"

/**
 * Number of bytes in the CTA header (tag + revision + DTD offset + flags).
 */
#define CTA_HEADER_SIZE 4
/**
 * Exclusive upper bound for the detailed timing definitions in the CTA block.
 */
#define CTA_DTD_END 127

static void
add_failure(struct di_edid_cta *cta, const char fmt[], ...)
{
	va_list args;

	va_start(args, fmt);
	_di_logger_va_add_failure(cta->logger, fmt, args);
	va_end(args);
}

static void
add_failure_until(struct di_edid_cta *cta, int revision, const char fmt[], ...)
{
	va_list args;

	if (cta->revision > revision) {
		return;
	}

	va_start(args, fmt);
	_di_logger_va_add_failure(cta->logger, fmt, args);
	va_end(args);
}

static bool
parse_video_block(struct di_edid_cta *cta, struct di_cta_video_block *video,
		  const uint8_t *data, size_t size)
{
	size_t i;
	uint8_t raw;
	struct di_cta_svd svd, *svd_ptr;

	if (size == 0)
		add_failure(cta, "Video Data Block: Empty Data Block");

	for (i = 0; i < size; i++) {
		raw = data[i];

		if (raw == 0 || raw == 128 || raw >= 254) {
			/* Reserved */
			add_failure_until(cta, 3,
					  "Video Data Block: Unknown VIC %" PRIu8 ".",
					  raw);
			continue;
		} else if (raw <= 127 || raw >= 193) {
			svd = (struct di_cta_svd) {
				.vic = raw,
			};
		} else {
			svd = (struct di_cta_svd) {
				.vic = get_bit_range(raw, 6, 0),
				.native = true,
			};
		}

		svd_ptr = calloc(1, sizeof(*svd_ptr));
		if (!svd_ptr)
			return false;
		*svd_ptr = svd;
		assert(video->svds_len < EDID_CTA_MAX_VIDEO_BLOCK_ENTRIES);
		video->svds[video->svds_len++] = svd_ptr;
	}

	return true;
}

static bool
parse_video_cap_block(struct di_edid_cta *cta,
		      struct di_cta_video_cap_block *video_cap,
		      const uint8_t *data, size_t size)
{
	if (size < 1) {
		add_failure(cta,
			    "Video Capability Data Block: Empty Data Block with length %u.",
			    size);
		return false;
	}

	video_cap->selectable_ycc_quantization_range = has_bit(data[0], 7);
	video_cap->selectable_rgb_quantization_range = has_bit(data[0], 6);
	video_cap->pt_over_underscan = get_bit_range(data[0], 5, 4);
	video_cap->it_over_underscan = get_bit_range(data[0], 3, 2);
	video_cap->ce_over_underscan = get_bit_range(data[0], 1, 0);

	if (!video_cap->selectable_rgb_quantization_range && cta->revision >= 3)
		add_failure(cta,
			    "Video Capability Data Block: Set Selectable RGB Quantization to avoid interop issues.");
	/* TODO: add failure if selectable_ycc_quantization_range is unset,
	 * the sink supports YCbCr formats and the revision is 3+ */

	switch (video_cap->it_over_underscan) {
	case DI_CTA_VIDEO_CAP_ALWAYS_OVERSCAN:
		if (cta->flags.it_underscan)
			add_failure(cta, "Video Capability Data Block: IT video formats are always overscanned, but bit 7 of Byte 3 of the CTA-861 Extension header is set to underscanned.");
		break;
	case DI_CTA_VIDEO_CAP_ALWAYS_UNDERSCAN:
		if (!cta->flags.it_underscan)
			add_failure(cta, "Video Capability Data Block: IT video formats are always underscanned, but bit 7 of Byte 3 of the CTA-861 Extension header is set to overscanned.");
	default:
		break;
	}

	return true;
}

static bool
parse_colorimetry_block(struct di_edid_cta *cta,
			struct di_cta_colorimetry_block *colorimetry,
			const uint8_t *data, size_t size)
{
	if (size < 2) {
		add_failure(cta, "Colorimetry Data Block: Empty Data Block with length %u.",
			    size);
		return false;
	}

	colorimetry->bt2020_rgb = has_bit(data[0], 7);
	colorimetry->bt2020_ycc = has_bit(data[0], 6);
	colorimetry->bt2020_cycc = has_bit(data[0], 5);
	colorimetry->oprgb = has_bit(data[0], 4);
	colorimetry->opycc_601 = has_bit(data[0], 3);
	colorimetry->sycc_601 = has_bit(data[0], 2);
	colorimetry->xvycc_709 = has_bit(data[0], 1);
	colorimetry->xvycc_601 = has_bit(data[0], 0);

	colorimetry->st2113_rgb = has_bit(data[1], 7);
	colorimetry->ictcp = has_bit(data[1], 6);

	if (get_bit_range(data[1], 5, 0) != 0)
		add_failure_until(cta, 3,
				  "Colorimetry Data Block: Reserved bits MD0-MD3 must be 0.");

	return true;
}

static float
parse_max_luminance(uint8_t raw)
{
	if (raw == 0)
		return 0;
	return 50 * powf(2, (float) raw / 32);
}

static float
parse_min_luminance(uint8_t raw, float max)
{
	if (raw == 0)
		return 0;
	return max * powf((float) raw / 255, 2) / 100;
}

static bool
parse_hdr_static_metadata_block(struct di_edid_cta *cta,
				struct di_cta_hdr_static_metadata_block_priv *metadata,
				const uint8_t *data, size_t size)
{
	uint8_t eotfs, descriptors;

	if (size < 2) {
		add_failure(cta, "HDR Static Metadata Data Block: Empty Data Block with length %u.",
			    size);
		return false;
	}

	metadata->base.eotfs = &metadata->eotfs;
	metadata->base.descriptors = &metadata->descriptors;

	eotfs = data[0];
	metadata->eotfs.traditional_sdr = has_bit(eotfs, 0);
	metadata->eotfs.traditional_hdr = has_bit(eotfs, 1);
	metadata->eotfs.pq = has_bit(eotfs, 2);
	metadata->eotfs.hlg = has_bit(eotfs, 3);
	if (get_bit_range(eotfs, 7, 4))
		add_failure_until(cta, 3, "HDR Static Metadata Data Block: Unknown EOTF.");

	descriptors = data[1];
	metadata->descriptors.type1 = has_bit(descriptors, 0);
	if (get_bit_range(descriptors, 7, 1))
		add_failure_until(cta, 3, "HDR Static Metadata Data Block: Unknown descriptor type.");

	if (size > 2)
		metadata->base.desired_content_max_luminance = parse_max_luminance(data[2]);
	if (size > 3)
		metadata->base.desired_content_max_frame_avg_luminance = parse_max_luminance(data[3]);
	if (size > 4) {
		if (metadata->base.desired_content_max_luminance == 0)
			add_failure(cta, "HDR Static Metadata Data Block: Desired content min luminance is set, but max luminance is unset.");
		else
			metadata->base.desired_content_min_luminance =
				parse_min_luminance(data[4], metadata->base.desired_content_max_luminance);
	}

	return true;
}

static bool
parse_vesa_transfer_characteristics_block(struct di_edid_cta *cta,
					  struct di_cta_vesa_transfer_characteristics *tf,
					  const uint8_t *data, size_t size)
{
	size_t i;

	if (size != 7 && size != 15 && size != 31) {
		add_failure(cta, "Invalid length %u.", size);
		return false;
	}

	tf->points_len = (uint8_t) size + 1;
	tf->usage = get_bit_range(data[0], 7, 6);

	tf->points[0] = get_bit_range(data[0], 5, 0) / 1023.0f;
	for (i = 1; i < size; i++)
		tf->points[i] = tf->points[i - 1] + data[i] / 1023.0f;
	tf->points[i] = 1.0f;

	return true;
}

static void
destroy_data_block(struct di_cta_data_block *data_block)
{
	size_t i;
	struct di_cta_video_block *video;

	switch (data_block->tag) {
	case DI_CTA_DATA_BLOCK_VIDEO:
		video = &data_block->video;
		for (i = 0; i < video->svds_len; i++)
			free(video->svds[i]);
		break;
	default:
		break; /* Nothing to do */
	}

	free(data_block);
}

static bool
parse_data_block(struct di_edid_cta *cta, uint8_t raw_tag, const uint8_t *data, size_t size)
{
	enum di_cta_data_block_tag tag;
	uint8_t extended_tag;
	struct di_cta_data_block *data_block;

	data_block = calloc(1, sizeof(*data_block));
	if (!data_block) {
		return false;
	}

	switch (raw_tag) {
	case 1:
		tag = DI_CTA_DATA_BLOCK_AUDIO;
		break;
	case 2:
		tag = DI_CTA_DATA_BLOCK_VIDEO;
		if (!parse_video_block(cta, &data_block->video, data, size))
			goto error;
		break;
	case 3:
		/* Vendor-Specific Data Block */
		goto skip;
	case 4:
		tag = DI_CTA_DATA_BLOCK_SPEAKER_ALLOC;
		break;
	case 5:
		tag = DI_CTA_DATA_BLOCK_VESA_DISPLAY_TRANSFER_CHARACTERISTIC;
		if (!parse_vesa_transfer_characteristics_block(cta,
							       &data_block->vesa_transfer_characteristics,
							       data, size))
			goto error;
		break;
	case 7:
		/* Use Extended Tag */
		if (size < 1) {
			add_failure(cta, "Empty block with extended tag.");
			goto skip;
		}

		extended_tag = data[0];
		data = &data[1];
		size--;

		switch (extended_tag) {
		case 0:
			tag = DI_CTA_DATA_BLOCK_VIDEO_CAP;
			if (!parse_video_cap_block(cta, &data_block->video_cap,
						   data, size))
				goto skip;
			break;
		case 2:
			tag = DI_CTA_DATA_BLOCK_VESA_DISPLAY_DEVICE;
			break;
		case 5:
			tag = DI_CTA_DATA_BLOCK_COLORIMETRY;
			if (!parse_colorimetry_block(cta,
						     &data_block->colorimetry,
						     data, size))
				goto skip;
			break;
		case 6:
			tag = DI_CTA_DATA_BLOCK_HDR_STATIC_METADATA;
			if (!parse_hdr_static_metadata_block(cta,
							     &data_block->hdr_static_metadata,
							     data, size))
				goto skip;
			break;
		case 7:
			tag = DI_CTA_DATA_BLOCK_HDR_DYNAMIC_METADATA;
			break;
		case 13:
			tag = DI_CTA_DATA_BLOCK_VIDEO_FORMAT_PREF;
			break;
		case 14:
			tag = DI_CTA_DATA_BLOCK_YCBCR420;
			break;
		case 15:
			tag = DI_CTA_DATA_BLOCK_YCBCR420_CAP_MAP;
			break;
		case 18:
			tag = DI_CTA_DATA_BLOCK_HDMI_AUDIO;
			break;
		case 19:
			tag = DI_CTA_DATA_BLOCK_ROOM_CONFIG;
			break;
		case 20:
			tag = DI_CTA_DATA_BLOCK_SPEAKER_LOCATION;
			break;
		case 32:
			tag = DI_CTA_DATA_BLOCK_INFOFRAME;
			break;
		case 34:
			tag = DI_CTA_DATA_BLOCK_DISPLAYID_VIDEO_TIMING_VII;
			break;
		case 35:
			tag = DI_CTA_DATA_BLOCK_DISPLAYID_VIDEO_TIMING_VIII;
			break;
		case 42:
			tag = DI_CTA_DATA_BLOCK_DISPLAYID_VIDEO_TIMING_X;
			break;
		case 120:
			tag = DI_CTA_DATA_BLOCK_HDMI_EDID_EXT_OVERRIDE;
			break;
		case 121:
			tag = DI_CTA_DATA_BLOCK_HDMI_SINK_CAP;
			break;
		case 1: /* Vendor-Specific Video Data Block */
		case 17: /* Vendor-Specific Audio Data Block */
			goto skip;
		default:
			/* Reserved */
			add_failure_until(cta, 3,
					  "Unknown CTA-861 Data Block (extended tag 0x"PRIx8", length %zu).",
					  extended_tag, size);
			goto skip;
		}
		break;
	default:
		/* Reserved */
		add_failure_until(cta, 3, "Unknown CTA-861 Data Block (tag 0x"PRIx8", length %zu).",
				  raw_tag, size);
		goto skip;
	}

	data_block->tag = tag;
	assert(cta->data_blocks_len < EDID_CTA_MAX_DATA_BLOCKS);
	cta->data_blocks[cta->data_blocks_len++] = data_block;
	return true;

skip:
	free(data_block);
	return true;

error:
	destroy_data_block(data_block);
	return false;
}

bool
_di_edid_cta_parse(struct di_edid_cta *cta, const uint8_t *data, size_t size,
		   struct di_logger *logger)
{
	uint8_t flags, dtd_start;
	uint8_t data_block_header, data_block_tag, data_block_size;
	size_t i;
	struct di_edid_detailed_timing_def_priv *detailed_timing_def;

	assert(size == 128);
	assert(data[0] == 0x02);

	cta->logger = logger;

	cta->revision = data[1];
	dtd_start = data[2];

	flags = data[3];
	if (cta->revision >= 2) {
		cta->flags.it_underscan = has_bit(flags, 7);
		cta->flags.basic_audio = has_bit(flags, 6);
		cta->flags.ycc444 = has_bit(flags, 5);
		cta->flags.ycc422 = has_bit(flags, 4);
		cta->flags.native_dtds = get_bit_range(flags, 3, 0);
	} else if (flags != 0) {
		/* Reserved */
		add_failure(cta, "Non-zero byte 3.");
	}

	if (dtd_start == 0) {
		return true;
	} else if (dtd_start < CTA_HEADER_SIZE || dtd_start >= size) {
		errno = EINVAL;
		return false;
	}

	i = CTA_HEADER_SIZE;
	while (i < dtd_start) {
		data_block_header = data[i];
		data_block_tag = get_bit_range(data_block_header, 7, 5);
		data_block_size = get_bit_range(data_block_header, 4, 0);

		if (i + 1 + data_block_size > dtd_start) {
			_di_edid_cta_finish(cta);
			errno = EINVAL;
			return false;
		}

		if (!parse_data_block(cta, data_block_tag,
				      &data[i + 1], data_block_size)) {
			_di_edid_cta_finish(cta);
			return false;
		}

		i += 1 + data_block_size;
	}

	if (i != dtd_start)
		add_failure(cta, "Offset is %"PRIu8", but should be %zu.",
			    dtd_start, i);

	for (i = dtd_start; i + EDID_BYTE_DESCRIPTOR_SIZE <= CTA_DTD_END;
	     i += EDID_BYTE_DESCRIPTOR_SIZE) {
		if (data[i] == 0)
			break;

		detailed_timing_def = _di_edid_parse_detailed_timing_def(&data[i]);
		if (!detailed_timing_def) {
			_di_edid_cta_finish(cta);
			return false;
		}
		assert(cta->detailed_timing_defs_len < EDID_CTA_MAX_DETAILED_TIMING_DEFS);
		cta->detailed_timing_defs[cta->detailed_timing_defs_len++] = detailed_timing_def;
	}

	/* All padding bytes after the last DTD must be zero */
	while (i < CTA_DTD_END) {
		if (data[i] != 0) {
			add_failure(cta, "Padding: Contains non-zero bytes.");
			break;
		}
		i++;
	}

	cta->logger = NULL;
	return true;
}

void
_di_edid_cta_finish(struct di_edid_cta *cta)
{
	size_t i;

	for (i = 0; i < cta->data_blocks_len; i++) {
		destroy_data_block(cta->data_blocks[i]);
	}

	for (i = 0; i < cta->detailed_timing_defs_len; i++) {
		free(cta->detailed_timing_defs[i]);
	}
}

int
di_edid_cta_get_revision(const struct di_edid_cta *cta)
{
	return cta->revision;
}

const struct di_edid_cta_flags *
di_edid_cta_get_flags(const struct di_edid_cta *cta)
{
	return &cta->flags;
}

const struct di_cta_data_block *const *
di_edid_cta_get_data_blocks(const struct di_edid_cta *cta)
{
	return (const struct di_cta_data_block *const *) cta->data_blocks;
}

enum di_cta_data_block_tag
di_cta_data_block_get_tag(const struct di_cta_data_block *block)
{
	return block->tag;
}

const struct di_cta_svd *const *
di_cta_data_block_get_svds(const struct di_cta_data_block *block)
{
	if (block->tag != DI_CTA_DATA_BLOCK_VIDEO) {
		return NULL;
	}
	return (const struct di_cta_svd *const *) block->video.svds;
}

const struct di_cta_colorimetry_block *
di_cta_data_block_get_colorimetry(const struct di_cta_data_block *block)
{
	if (block->tag != DI_CTA_DATA_BLOCK_COLORIMETRY) {
		return NULL;
	}
	return &block->colorimetry;
}

const struct di_cta_hdr_static_metadata_block *
di_cta_data_block_get_hdr_static_metadata(const struct di_cta_data_block *block)
{
	if (block->tag != DI_CTA_DATA_BLOCK_HDR_STATIC_METADATA) {
		return NULL;
	}
	return &block->hdr_static_metadata.base;
}

const struct di_cta_video_cap_block *
di_cta_data_block_get_video_cap(const struct di_cta_data_block *block)
{
	if (block->tag != DI_CTA_DATA_BLOCK_VIDEO_CAP) {
		return NULL;
	}
	return &block->video_cap;
}

const struct di_edid_detailed_timing_def *const *
di_edid_cta_get_detailed_timing_defs(const struct di_edid_cta *cta)
{
	return (const struct di_edid_detailed_timing_def *const *) cta->detailed_timing_defs;
}

const struct di_cta_vesa_transfer_characteristics *
di_cta_data_block_get_vesa_transfer_characteristics(const struct di_cta_data_block *block)
{
	if (block->tag != DI_CTA_DATA_BLOCK_VESA_DISPLAY_TRANSFER_CHARACTERISTIC) {
		return NULL;
	}
	return &block->vesa_transfer_characteristics;
}
