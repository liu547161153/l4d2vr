#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <libdisplay-info/cta.h>

#include "di-edid-decode.h"

static void
printf_cta_svds(const struct di_cta_svd *const *svds)
{
	size_t i;
	const struct di_cta_svd *svd;

	for (i = 0; svds[i] != NULL; i++) {
		svd = svds[i];

		printf("    VIC %3" PRIu8, svd->vic);
		if (svd->native)
			printf(" (native)");
		printf("\n");

		// TODO: print detailed mode info
	}
}

static uint8_t
encode_max_luminance(float max)
{
	if (max == 0)
		return 0;
	return (uint8_t) (log2f(max / 50) * 32);
}

static uint8_t
encode_min_luminance(float min, float max)
{
	if (min == 0)
		return 0;
	return (uint8_t) (255 * sqrtf(min / max * 100));
}

static void
print_cta_hdr_static_metadata(const struct di_cta_hdr_static_metadata_block *metadata)
{
	printf("    Electro optical transfer functions:\n");
	if (metadata->eotfs->traditional_sdr)
		printf("      Traditional gamma - SDR luminance range\n");
	if (metadata->eotfs->traditional_hdr)
		printf("      Traditional gamma - HDR luminance range\n");
	if (metadata->eotfs->pq)
		printf("      SMPTE ST2084\n");
	if (metadata->eotfs->hlg)
		printf("      Hybrid Log-Gamma\n");

	printf("    Supported static metadata descriptors:\n");
	if (metadata->descriptors->type1)
		printf("      Static metadata type 1\n");

	/* TODO: figure out a way to print raw values? */
	if (metadata->desired_content_max_luminance != 0)
		printf("    Desired content max luminance: %" PRIu8 " (%.3f cd/m^2)\n",
		       encode_max_luminance(metadata->desired_content_max_luminance),
		       metadata->desired_content_max_luminance);
	if (metadata->desired_content_max_frame_avg_luminance != 0)
		printf("    Desired content max frame-average luminance: %" PRIu8 " (%.3f cd/m^2)\n",
		       encode_max_luminance(metadata->desired_content_max_frame_avg_luminance),
		       metadata->desired_content_max_frame_avg_luminance);
	if (metadata->desired_content_min_luminance != 0)
		printf("    Desired content min luminance: %" PRIu8 " (%.3f cd/m^2)\n",
		       encode_min_luminance(metadata->desired_content_min_luminance,
					    metadata->desired_content_max_luminance),
		       metadata->desired_content_min_luminance);
}

static void
print_cta_vesa_transfer_characteristics(const struct di_cta_vesa_transfer_characteristics *tf)
{
	size_t i;

	switch (tf->usage) {
	case DI_CTA_VESA_TRANSFER_CHARACTERISTIC_USAGE_WHITE:
		printf("    White");
		break;
	case DI_CTA_VESA_TRANSFER_CHARACTERISTIC_USAGE_RED:
		printf("    Red");
		break;
	case DI_CTA_VESA_TRANSFER_CHARACTERISTIC_USAGE_GREEN:
		printf("    Green");
		break;
	case DI_CTA_VESA_TRANSFER_CHARACTERISTIC_USAGE_BLUE:
		printf("    Blue");
		break;
	}

	printf(" transfer characteristics:");
	for (i = 0; i < tf->points_len; i++)
		printf(" %u", (uint16_t) roundf(tf->points[i] * 1023.0f));
	printf("\n");

	uncommon_features.cta_transfer_characteristics = true;
}

static const char *
cta_data_block_tag_name(enum di_cta_data_block_tag tag)
{
	switch (tag) {
	case DI_CTA_DATA_BLOCK_AUDIO:
		return "Audio Data Block";
	case DI_CTA_DATA_BLOCK_VIDEO:
		return "Video Data Block";
	case DI_CTA_DATA_BLOCK_SPEAKER_ALLOC:
		return "Speaker Allocation Data Block";
	case DI_CTA_DATA_BLOCK_VESA_DISPLAY_TRANSFER_CHARACTERISTIC:
		return "VESA Display Transfer Characteristics Data Block";
	case DI_CTA_DATA_BLOCK_VIDEO_CAP:
		return "Video Capability Data Block";
	case DI_CTA_DATA_BLOCK_VESA_DISPLAY_DEVICE:
		return "VESA Video Display Device Data Block";
	case DI_CTA_DATA_BLOCK_COLORIMETRY:
		return "Colorimetry Data Block";
	case DI_CTA_DATA_BLOCK_HDR_STATIC_METADATA:
		return "HDR Static Metadata Data Block";
	case DI_CTA_DATA_BLOCK_HDR_DYNAMIC_METADATA:
		return "HDR Dynamic Metadata Data Block";
	case DI_CTA_DATA_BLOCK_VIDEO_FORMAT_PREF:
		return "Video Format Preference Data Block";
	case DI_CTA_DATA_BLOCK_YCBCR420:
		return "YCbCr 4:2:0 Video Data Block";
	case DI_CTA_DATA_BLOCK_YCBCR420_CAP_MAP:
		return "YCbCr 4:2:0 Capability Map Data Block";
	case DI_CTA_DATA_BLOCK_HDMI_AUDIO:
		return "HDMI Audio Data Block";
	case DI_CTA_DATA_BLOCK_ROOM_CONFIG:
		return "Room Configuration Data Block";
	case DI_CTA_DATA_BLOCK_SPEAKER_LOCATION:
		return "Speaker Location Data Block";
	case DI_CTA_DATA_BLOCK_INFOFRAME:
		return "InfoFrame Data Block";
	case DI_CTA_DATA_BLOCK_DISPLAYID_VIDEO_TIMING_VII:
		return "DisplayID Type VII Video Timing Data Block";
	case DI_CTA_DATA_BLOCK_DISPLAYID_VIDEO_TIMING_VIII:
		return "DisplayID Type VIII Video Timing Data Block";
	case DI_CTA_DATA_BLOCK_DISPLAYID_VIDEO_TIMING_X:
		return "DisplayID Type X Video Timing Data Block";
	case DI_CTA_DATA_BLOCK_HDMI_EDID_EXT_OVERRIDE :
		return "HDMI Forum EDID Extension Override Data Block";
	case DI_CTA_DATA_BLOCK_HDMI_SINK_CAP:
		return "HDMI Forum Sink Capability Data Block";
	}
	return "Unknown CTA-861 Data Block";
}

static const char *
video_cap_over_underscan_name(enum di_cta_video_cap_over_underscan over_underscan,
			      const char *unknown)
{
	switch (over_underscan) {
	case DI_CTA_VIDEO_CAP_UNKNOWN_OVER_UNDERSCAN:
		return unknown;
	case DI_CTA_VIDEO_CAP_ALWAYS_OVERSCAN:
		return "Always Overscanned";
	case DI_CTA_VIDEO_CAP_ALWAYS_UNDERSCAN:
		return "Always Underscanned";
	case DI_CTA_VIDEO_CAP_BOTH_OVER_UNDERSCAN:
		return "Supports both over- and underscan";
	}
	abort();
}

void
print_cta(const struct di_edid_cta *cta)
{
	const struct di_edid_cta_flags *cta_flags;
	const struct di_cta_data_block *const *data_blocks;
	const struct di_cta_data_block *data_block;
	enum di_cta_data_block_tag data_block_tag;
	const struct di_cta_svd *const *svds;
	const struct di_cta_video_cap_block *video_cap;
	const struct di_cta_colorimetry_block *colorimetry;
	const struct di_cta_hdr_static_metadata_block *hdr_static_metadata;
	const struct di_cta_vesa_transfer_characteristics *transfer_characteristics;
	size_t i;
	const struct di_edid_detailed_timing_def *const *detailed_timing_defs;

	printf("  Revision: %d\n", di_edid_cta_get_revision(cta));

	cta_flags = di_edid_cta_get_flags(cta);
	if (cta_flags->it_underscan) {
		printf("  Underscans IT Video Formats by default\n");
	}
	if (cta_flags->basic_audio) {
		printf("  Basic audio support\n");
	}
	if (cta_flags->ycc444) {
		printf("  Supports YCbCr 4:4:4\n");
	}
	if (cta_flags->ycc422) {
		printf("  Supports YCbCr 4:2:2\n");
	}
	printf("  Native detailed modes: %d\n", cta_flags->native_dtds);

	data_blocks = di_edid_cta_get_data_blocks(cta);
	for (i = 0; data_blocks[i] != NULL; i++) {
		data_block = data_blocks[i];

		data_block_tag = di_cta_data_block_get_tag(data_block);
		printf("  %s:\n", cta_data_block_tag_name(data_block_tag));

		switch (data_block_tag) {
		case DI_CTA_DATA_BLOCK_VIDEO:
			svds = di_cta_data_block_get_svds(data_block);
			printf_cta_svds(svds);
			break;
		case DI_CTA_DATA_BLOCK_VIDEO_CAP:
			video_cap = di_cta_data_block_get_video_cap(data_block);
			printf("    YCbCr quantization: %s\n",
			       video_cap->selectable_ycc_quantization_range ?
			       "Selectable (via AVI YQ)" : "No Data");
			printf("    RGB quantization: %s\n",
			       video_cap->selectable_ycc_quantization_range ?
			       "Selectable (via AVI Q)" : "No Data");
			printf("    PT scan behavior: %s\n",
			       video_cap_over_underscan_name(video_cap->pt_over_underscan,
							     "No Data"));
			printf("    IT scan behavior: %s\n",
			       video_cap_over_underscan_name(video_cap->it_over_underscan,
							     "IT video formats not supported"));
			printf("    CE scan behavior: %s\n",
			       video_cap_over_underscan_name(video_cap->ce_over_underscan,
							     "CE video formats not supported"));
			break;
		case DI_CTA_DATA_BLOCK_COLORIMETRY:
			colorimetry = di_cta_data_block_get_colorimetry(data_block);
			if (colorimetry->xvycc_601)
				printf("    xvYCC601\n");
			if (colorimetry->xvycc_709)
				printf("    xvYCC709\n");
			if (colorimetry->sycc_601)
				printf("    sYCC601\n");
			if (colorimetry->opycc_601)
				printf("    opYCC601\n");
			if (colorimetry->oprgb)
				printf("    opRGB\n");
			if (colorimetry->bt2020_cycc)
				printf("    BT2020cYCC\n");
			if (colorimetry->bt2020_ycc)
				printf("    BT2020YCC\n");
			if (colorimetry->bt2020_rgb)
				printf("    BT2020RGB\n");
			if (colorimetry->ictcp)
				printf("    ICtCp\n");
			if (colorimetry->st2113_rgb)
				printf("    ST2113RGB\n");
			break;
		case DI_CTA_DATA_BLOCK_HDR_STATIC_METADATA:
			hdr_static_metadata = di_cta_data_block_get_hdr_static_metadata(data_block);
			print_cta_hdr_static_metadata(hdr_static_metadata);
			break;
		case DI_CTA_DATA_BLOCK_VESA_DISPLAY_TRANSFER_CHARACTERISTIC:
			transfer_characteristics = di_cta_data_block_get_vesa_transfer_characteristics(data_block);
			print_cta_vesa_transfer_characteristics(transfer_characteristics);
			break;
		default:
			break; /* Ignore */
		}
	}

	detailed_timing_defs = di_edid_cta_get_detailed_timing_defs(cta);
	if (detailed_timing_defs[0]) {
		printf("  Detailed Timing Descriptors:\n");
	}
	for (i = 0; detailed_timing_defs[i] != NULL; i++) {
		print_detailed_timing_def(detailed_timing_defs[i]);
	}
}
