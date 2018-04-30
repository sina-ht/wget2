/*
 * Copyright(c) 2017-2018 Free Software Foundation, Inc.
 *
 * This file is part of libwget.
 *
 * Libwget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libwget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libwget.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <assert.h> // assert
#include <stdlib.h> // malloc, free
#include <string.h> // memcpy

#include "wget.h"
#include "fuzzer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size > 100) // same as max_len = 100 in .options file
		return 0;

	char *in = (char *)malloc(size + 1);

	assert(in != NULL);

	// 0 terminate
	memcpy(in, data, size);
	in[size] = 0;

	if (wget_base64_is_string(NULL) || wget_base64_is_string(in))
		in[size] = 0;

	free(wget_base64_decode_alloc((char *) data, size, NULL));
	free(wget_base64_encode_printf_alloc("%s", in));

	free(in);

	// just to cover inline function in wget.h
	wget_base64_get_decoded_length(5);
	wget_base64_get_encoded_length(5);

	return 0;
}
