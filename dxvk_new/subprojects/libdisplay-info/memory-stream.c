#include <stdlib.h>

#include "memory-stream.h"

#ifndef _WIN32

bool
memory_stream_open(struct memory_stream *m)
{
	*m = (struct memory_stream){ 0 };
	m->fp = open_memstream(&m->str, &m->str_len);

	return m->fp != NULL;
}

char *
memory_stream_close(struct memory_stream *m)
{
	char *str;
	int ret;

	ret = fclose(m->fp);
	str = m->str;
	*m = (struct memory_stream){ 0 };

	if (ret != 0) {
		free(str);
		str = NULL;
	}

	return str;
}

#else

#include <windows.h>

bool
memory_stream_open(struct memory_stream *m)
{
	char path[MAX_PATH];
	DWORD dwResult;

	*m = (struct memory_stream){ 0 };

	dwResult = GetTempPath(MAX_PATH, path);
	if ((dwResult > 0) && (dwResult < MAX_PATH)) {
		char *temp = m->temp;
		UINT uResult = GetTempFileName(path, "MEMSTREAM", 0, temp);
		if (uResult != 0) {
			FILE *f = fopen(temp, "w+b");
			if (f != NULL) {
				m->fp = f;
				return true;
			}
		}
	}

   return false;
}

char *
memory_stream_close(struct memory_stream *m)
{
	size_t size;
	char* str;

	if (m->fp == NULL) {
		*m = (struct memory_stream){ 0 };
		return NULL;
	}

	_fseeki64(m->fp, 0ll, SEEK_END);
	size = (size_t)_ftelli64(m->fp);
	_fseeki64(m->fp, 0ll, SEEK_SET);

	str  = malloc(size + 1);
	if (str == NULL) {
		fclose(m->fp);

		remove(m->temp);
		*m = (struct memory_stream){ 0 };
		return NULL;
	}

	fread(str, 1, size, m->fp);
	str[size] = '\0';
	fclose(m->fp);

	remove(m->temp);
	*m = (struct memory_stream){ 0 };

	return str;
}

#endif
