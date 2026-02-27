/*
command-line tool to extract GPS time and position telemetry from GoPro videos
Copyright (C) 2021 Peter Lawrence

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include <math.h>

#include "./gpmf-parser/GPMF_parser.h"
#include "./gpmf-parser/demo/GPMF_mp4reader.h"
#include "./gpmf-parser/GPMF_utils.h"

static const char *const column_names[] =
{
	"file",
	"cts",
	"date",
	"GPS (Lat.) [deg]",
	"GPS (Long.) [deg]",
	"GPS (Alt.) [m]",
	"GPS (2D speed) [m/s]",
	"GPS (3D speed) [m/s]",
	"fix","precision",
};

static const int gps9_indexes[] =
{
	0, /* lat */
	1, /* long */
	2, /* alt */
	3, /* 2D speed */
	4, /* 3D speed */
	8, /* fix */
	7, /* precision */
};

int main(int argc, char* argv[])
{
	GPMF_ERR ret = GPMF_OK;
	GPMF_stream metadata_stream, *ms = &metadata_stream;
	double file_start = 0.0;
	time_t gps9_epoch;
	struct tm tm;
	bool use_gps9 = false;
	int min_fix = -1; /* -1 means no filtering */
	int max_precision = -1; /* -1 means no filtering */
	bool print_filename = false;
	bool print_filepath = false;

	if (argc < 2)
	{
		fprintf(stderr, "%s [options] <mp4file> [mp4file_2] ... [mp4file_n]\n", argv[0]);
		fprintf(stderr, "  --print_filename   print the filename in output\n");
		fprintf(stderr, "  --print_filepath   print the full file path in output\n");
		fprintf(stderr, "  --min_fix=N        only output entries with fix >= N\n");
		fprintf(stderr, "  --max_precision=N  only output entries with precision <= N\n");
		return -1;
	}

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 100;
	gps9_epoch = timegm(&tm);

	/* check for filter parameters */
	int first_file_index = 1;
	while (first_file_index < argc)
	{
		if (strcmp(argv[first_file_index], "--print_filename") == 0)
		{
			print_filename = true;
			first_file_index++;
		}
		else if (strcmp(argv[first_file_index], "--print_filepath") == 0)
		{
			print_filepath = true;
			first_file_index++;
		}
		else if (strncmp(argv[first_file_index], "--min_fix=", 10) == 0)
		{
			min_fix = atoi(argv[first_file_index] + 10);
			first_file_index++;
		}
		else if (strncmp(argv[first_file_index], "--max_precision=", 16) == 0)
		{
			max_precision = atoi(argv[first_file_index] + 16);
			first_file_index++;
		}
		else
		{
			break; /* not a parameter, must be a filename */
		}
	}

	if (first_file_index >= argc)
	{
		fprintf(stderr, "%s [options] <mp4file> [mp4file_2] ... [mp4file_n]\n", argv[0]);
		return -1;
	}

	for (int argc_index = first_file_index; argc_index < argc; argc_index++)
	{
		char *mp4filename = argv[argc_index];
		/* extract just the filename from the path */
		const char *display_name = strrchr(mp4filename, '/');
		display_name = display_name ? display_name + 1 : mp4filename;

		/* search for GPMF Track */
		size_t mp4handle = OpenMP4Source(mp4filename, MOV_GPMF_TRAK_TYPE, MOV_GPMF_TRAK_SUBTYPE, 0);

		if (mp4handle == 0)
		{
			fprintf(stderr, "ERROR: %s is an invalid MP4/MOV or it has no GPMF data\n\n", mp4filename);
			return -1;
		}

		double metadataduration = GetDuration(mp4handle);
		if (metadataduration <= 0.0) return -1;

		if (first_file_index == argc_index)
		{
			/* print column names on the first row */
			int col = 0;
			if (print_filename || print_filepath)
				printf("\"%s\"", column_names[0]); /* "file" */
			for (int i = 1; i < (sizeof(column_names) / sizeof(*column_names)); i++)
				printf("%s\"%s\"", (col++ || print_filename || print_filepath) ? "," : "", column_names[i]);
			printf("\n");
		}

		size_t payloadres = 0;
		double file_finish = 0.0;

		/* each MP4 has a given number of payloads, and we must iterate through all of them */
		uint32_t payloads = GetNumberPayloads(mp4handle);

		for (uint32_t index = 0; index < payloads; index++)
		{
			double start = 0.0, finish = 0.0;
			uint32_t payloadsize = GetPayloadSize(mp4handle, index);
			payloadres = GetPayloadResource(mp4handle, payloadres, payloadsize);

			uint32_t *payload = GetPayload(mp4handle, payloadres, index);
			if (payload == NULL) break;

			ret = GetPayloadTime(mp4handle, index, &start, &finish);
			if (ret != GPMF_OK) break;

			ret = GPMF_Init(ms, payload, payloadsize);
			if (ret != GPMF_OK) break;

			uint32_t fix;       /* data from "GPSF" */
			uint16_t precision; /* data from "GPSP" */
			struct              /* data from "GPSU" */
			{
				time_t time; /* second-accurate standard format compatible with time.h routines */
				double milliseconds; /* sub-second quantity to add to the above time_t data */
			} gpsu;

			/* iterate through all GPMF data in this particular payload */
			do
			{
				uint32_t key = GPMF_Key(ms);
				uint32_t samples = GPMF_Repeat(ms);
				uint32_t elements = GPMF_ElementsInStruct(ms);

				if (!samples) continue;

				uint32_t structsize = GPMF_StructSize(ms);

				if (!structsize) continue;

				uint32_t buffersize = samples * elements * structsize;
				void *tmpbuffer = malloc(buffersize);

				if (!tmpbuffer) continue;

				int res = GPMF_ERROR_UNKNOWN_TYPE;
				if ( (STR2FOURCC("GPSU") == key) || (STR2FOURCC("GPSF") == key) | (STR2FOURCC("GPSP") == key) )
					res = GPMF_FormattedData(ms, tmpbuffer, buffersize, 0, samples);
				else if (STR2FOURCC("GPS5") == key)
					res = GPMF_ScaledData(ms, tmpbuffer, buffersize, 0, samples, GPMF_TYPE_DOUBLE);
				else if (STR2FOURCC("GPS9") == key)
					res = GPMF_ScaledData(ms, tmpbuffer, buffersize, 0, samples, GPMF_TYPE_DOUBLE);

				if (GPMF_OK == res)
				{
					double *ptr = tmpbuffer;
					double step = (finish - start) / (double)samples;;
					double now = start;

					file_finish = finish;

					for (uint32_t i = 0; i < samples; i++)
					{
						if (STR2FOURCC("GPSU") == key)
						{
							char *gpsu_string = tmpbuffer;
							struct tm tm;
							memset(&tm, 0, sizeof(tm));

							/* GoPro provides the time as a fixed-size ASCII string, which we must convert to something useable */
							tm.tm_year  = 10 * (gpsu_string[0]  - '0') + (gpsu_string[1]  - '0');
							tm.tm_year += 100;
							tm.tm_mon   = 10 * (gpsu_string[2]  - '0') + (gpsu_string[3]  - '0');
							tm.tm_mon--; /* struct tm uses an ordinal month */
							tm.tm_mday  = 10 * (gpsu_string[4]  - '0') + (gpsu_string[5]  - '0');
							tm.tm_hour  = 10 * (gpsu_string[6]  - '0') + (gpsu_string[7]  - '0');
							tm.tm_min   = 10 * (gpsu_string[8]  - '0') + (gpsu_string[9]  - '0');
							tm.tm_sec   = 10 * (gpsu_string[10] - '0') + (gpsu_string[11] - '0');
							gpsu.time         = timegm(&tm);
							gpsu.milliseconds = 100.0 * (gpsu_string[13] - '0') + 10.0 * (gpsu_string[14] - '0') + (gpsu_string[15] - '0');
						}
						else if ( (STR2FOURCC("GPS5") == key) && !use_gps9 )
						{
							/* at this point, we should have all the data (with "GPS5" being at the highest sample rate) */

							/* apply filters if specified */
							if ((min_fix < 0 || (int)fix >= min_fix) &&
							    (max_precision < 0 || (int)precision <= max_precision))
							{
								/* we print the filename (if requested) and time... */
								if (print_filepath)
									printf("\"%s\", ", mp4filename);
								else if (print_filename)
									printf("\"%s\", ", display_name);
								printf("%f, ", (file_start + now) * 1000.0);
								char ftimestr[64];
								strftime(ftimestr, sizeof(ftimestr), "%Y-%m-%dT%H:%M:%S", gmtime(&gpsu.time));
								printf("%s.%03dZ, ", ftimestr, (int)gpsu.milliseconds);

								/* ... and print out all the data */
								for (uint32_t j = 0; j < elements; j++)
									printf("%.6f, ", *ptr++);
								printf("%d, %d\n", fix, precision);
							}
							else
							{
								ptr += elements; /* skip this sample's data */
							}

							/*
							the time increment potentially rolls over into the next minute, hour, or even day
							storing the second data in time_t makes our job much easier as strftime() above handles this
							*/
							now += step; gpsu.milliseconds += step * 1000.0;
							if (gpsu.milliseconds >= 1000.0)
							{
								gpsu.milliseconds -= 1000.0;
								gpsu.time++;
							}
						}
						else if (STR2FOURCC("GPS9") == key)
						{
							use_gps9 = true;
							int gps9_fix = (int)ptr[8]; /* fix is at index 8 in GPS9 */
							int gps9_precision = (int)ptr[7]; /* precision is at index 7 in GPS9 */

							/* apply filters if specified */
							if ((min_fix < 0 || gps9_fix >= min_fix) &&
							    (max_precision < 0 || gps9_precision <= max_precision))
							{
								/* we print the filename (if requested) and time... */
								if (print_filepath)
									printf("\"%s\", ", mp4filename);
								else if (print_filename)
									printf("\"%s\", ", display_name);
								printf("%f, ", (file_start + now) * 1000.0);
								char ftimestr[64];
								if (0.0 == now)
								{
									gpsu.time = gps9_epoch + /* days since 2000 */ ((time_t)ptr[5] + 1) * /* secs per day */ 86400;
									double sub_secs = fmod(ptr[6], 1.0);
									gpsu.milliseconds = (int)(1000.0 * sub_secs);
									gpsu.time += (time_t)(ptr[6] - sub_secs);
								}
								strftime(ftimestr, sizeof(ftimestr), "%Y-%m-%dT%H:%M:%S", gmtime(&gpsu.time));
								printf("%s.%03dZ", ftimestr, (int)gpsu.milliseconds);
								for (uint32_t j = 0; j < (sizeof(gps9_indexes) / sizeof(*gps9_indexes)); j++)
									printf(", %.6f", ptr[gps9_indexes[j]]);
								printf("\n");
							}

							ptr += elements; /* advance to next sample */
							now += step; gpsu.milliseconds += step * 1000.0;
							if (gpsu.milliseconds >= 1000.0)
							{
								gpsu.milliseconds -= 1000.0;
								gpsu.time++;
							}
						}
						else if (STR2FOURCC("GPSF") == key)
						{
							fix = *(uint32_t *)tmpbuffer;
						}
						else if (STR2FOURCC("GPSP") == key)
						{
							precision = *(uint16_t *)tmpbuffer;
						}
					}
				}

				free(tmpbuffer);

			} while (GPMF_OK == GPMF_Next(ms, GPMF_RECURSE_LEVELS));

			GPMF_ResetState(ms);
		}

		if (payloadres) FreePayloadResource(mp4handle, payloadres);
		if (ms) GPMF_Free(ms);
		CloseSource(mp4handle);

		if (ret != GPMF_OK)
		{
			if (GPMF_ERROR_UNKNOWN_TYPE == ret)
				fprintf(stderr, "ERROR: Unknown GPMF Type within\n");
			else
				fprintf(stderr, "ERROR: GPMF data has corruption\n");
			break;
		}

		file_start += file_finish;
	}

	return (int)ret;
}
