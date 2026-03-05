#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <libdisplay-info/info.h>

static const char *
str_or_null(const char *str)
{
	return str ? str : "{null}";
}

static void
print_info(const struct di_info *info)
{
	char *str;

	str = di_info_get_make(info);
	printf("make: %s\n", str_or_null(str));
	free(str);

	str = di_info_get_model(info);
	printf("model: %s\n", str_or_null(str));
	free(str);

	str = di_info_get_serial(info);
	printf("serial: %s\n", str_or_null(str));
	free(str);
}

int
main(int argc, char *argv[])
{
	FILE *in;
	static uint8_t raw[32 * 1024];
	size_t size = 0;
	struct di_info *info;

	in = stdin;
	if (argc > 1) {
		in = fopen(argv[1], "r");
		if (!in) {
			perror("failed to open input file");
			return 1;
		}
	}

	while (!feof(in)) {
		size += fread(&raw[size], 1, sizeof(raw) - size, in);
		if (ferror(in)) {
			perror("fread failed");
			return 1;
		} else if (size >= sizeof(raw)) {
			fprintf(stderr, "input too large\n");
			return 1;
		}
	}

	fclose(in);

	info = di_info_parse_edid(raw, size);
	if (!info) {
		perror("di_edid_parse failed");
		return 1;
	}

	print_info(info);
	di_info_destroy(info);

	return 0;
}
