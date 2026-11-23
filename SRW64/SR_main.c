/**
 *
 *  Copyright (C) 2016-2025 Roman Pauer
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#if (defined(__WIN32__) || defined(__WINDOWS__)) && !defined(_WIN32)
#define _WIN32
#endif

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN

	#include <windows.h>
#else
	#include <unistd.h>
#endif

#include "SR_defs.h"

#define DEFINE_VARIABLES
	#include "SR_vars.h"
#undef DEFINE_VARIABLES

#if defined(_WIN32)

static void write_log_time(FILE *fout)
{
	SYSTEMTIME st;

	GetSystemTime(&st);

	fprintf(fout, "%04i-%02i-%02i %02i:%02i:%02i.%03i: ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

#else
	#include <sys/time.h>

static void write_log_time(FILE *fout)
{
	struct tm *newtime;
	struct timeval current_time;
	time_t current_seconds;
	char str[63];

	gettimeofday(&current_time, NULL);						// Get time in seconds and microseconds
	current_seconds = current_time.tv_sec;
	newtime = gmtime( &current_seconds );					// Convert time (seconds) to struct
	strftime (str, 63, "%Y-%m-%d %H:%M:%S", newtime);		// Write date and time (seconds) to string (UTC)

	fprintf(fout, "%s.%03i: ", str, (int) (current_time.tv_usec / 1000));
}

#endif


int SR_get_section_reladr_exact(uint64_t Address, uint_fast32_t *SecNum, uint_fast32_t *RelAdr)
{
	unsigned int Entry;

	for (Entry = 0; Entry < num_sections; Entry++)
	{
		// uninitialized thread local storage (ELF .tbss) doesn't occupy address space
		if (section[Entry].tls && section[Entry].type == ST_UDATA) continue;

		if (Address >= section[Entry].start &&
			Address < section[Entry].start + section[Entry].size)
		{
			*SecNum = Entry;
			*RelAdr = (uint_fast32_t)(Address - section[Entry].start);

			return 1;
		}
	}
	for (Entry = 0; Entry < num_sections; Entry++)
	{
		if (section[Entry].tls && section[Entry].type == ST_UDATA) continue;

		if (Address == section[Entry].start + section[Entry].size)
		{
			*SecNum = Entry;
			*RelAdr = (uint_fast32_t)(Address - section[Entry].start);

			return 1;
		}
	}

	return 0;
}

int SR_get_section_reladr(uint64_t Address, uint_fast32_t *SecNum, uint_fast32_t *RelAdr)
{
	unsigned int Entry, Best;

	if (SR_get_section_reladr_exact(Address, SecNum, RelAdr)) return 1;

	// address between sections - use the nearest lower section
	Best = num_sections;
	for (Entry = 0; Entry < num_sections; Entry++)
	{
		if (section[Entry].tls && section[Entry].type == ST_UDATA) continue;

		if (Address >= section[Entry].start && Address < ImageBase + SizeOfImage)
		{
			if (Best == num_sections || section[Entry].start > section[Best].start)
			{
				Best = Entry;
			}
		}
	}

	if (Best != num_sections)
	{
		*SecNum = Best;
		*RelAdr = (uint_fast32_t)(Address - section[Best].start);

		return 1;
	}

	*SecNum = 0;
	*RelAdr = 0;
	return 0;
}

int SR_is_ignored(uint_fast32_t SecNum, uint_fast32_t RelAdr)
{
	int *ignored_area_value;
	Word_t ignored_area_index;

	ignored_area_value = section_ignored_list_FindEntryEqualOrLowerIndex(SecNum, RelAdr, &ignored_area_index);
	if (ignored_area_value != NULL)
	{
		if ((uint_fast32_t)(ignored_area_index + *ignored_area_value) > RelAdr)
		{
			return 1;
		}
	}

	return 0;
}

static char *trim_string(char *buf)
{
    size_t len;

    while (*buf == ' ') buf++;

    len = strlen(buf);
    if (len != 0 && buf[len - 1] == '\r')
    {
        len--;
        buf[len] = 0;
    }

    while (len != 0 && buf[len - 1] == ' ')
    {
        len--;
        buf[len] = 0;
    }

    return buf;
}

static void read_cfg_yesno(const char *param, int *value)
{
    if ( strcasecmp(param, "yes") == 0 ) /* param equals "yes" */
    {
        *value = 1;
    }
    else if ( strcasecmp(param, "no") == 0 ) /* param equals "no" */
    {
        *value = 0;
    }
}

static void read_cfg(void)
{
    const static char config_filename[] = "SR.cfg";

    FILE *f;
    char buf[8192];
    char *str, *param;
    int items;

    f = fopen(config_filename, "rt");

    if (f == NULL) return;

    while (!feof(f))
    {
        /* skip empty lines */
        items = fscanf(f, "%8191[\n\r]", buf);

        /* read line */
        buf[0] = 0;
        items = fscanf(f, "%8191[^\n^\r]", buf);
        if (items <= 0) continue;

        /* trim line */
        str = trim_string(buf);

        if (str[0] == 0) continue;
        if (str[0] == '#') continue;

        /* find parameter (after '=') */
        param = strchr(str, '=');

        if (param == NULL) continue;

        /* split string into two strings */
        *param = 0;
        param++;

        /* trim them */
        str = trim_string(str);
        param = trim_string(param);

        if ( strcasecmp(str, "use_pdata") == 0 )
        {
            read_cfg_yesno(param, &use_pdata);
        }
        else if ( strcasecmp(str, "convert_exception_data") == 0 )
        {
            read_cfg_yesno(param, &convert_exception_data);
        }
        else if ( strcasecmp(str, "detect_jump_tables") == 0 )
        {
            read_cfg_yesno(param, &detect_jump_tables);
        }
        else if ( strcasecmp(str, "detect_image_relative") == 0 )
        {
            read_cfg_yesno(param, &detect_image_relative);
        }
        else if ( strcasecmp(str, "keep_original_code") == 0 )
        {
            read_cfg_yesno(param, &keep_original_code);
        }
    }

    fclose(f);
}

static void deinitialize_values(void)
{
	unsigned int Entry;

	for (Entry = 0; Entry < num_sections; Entry++)
	{
		section_fixup_list_Free(Entry);
		section_output_list_Free(Entry);
		section_entry_list_Free(Entry);
		section_code_list_Free(Entry);
		section_label_list_Free(Entry);
		section_noret_list_Free(Entry);
		section_extrn_list_Free(Entry);
		section_alias_list_Free(Entry);
		section_ignored_list_Free(Entry);
		section_replace_list_Free(Entry);
		section_nocode_list_Free(Entry);
		section_export_list_Free(Entry);
		section_ibreg_list_Free(Entry);
		section_hint_list_Free(Entry);
		section_rvadisp_list_Free(Entry);
	}
	num_sections = 0;

	import_list_Free();

	if (pdata_list != NULL)
	{
		free(pdata_list);
		pdata_list = NULL;
	}
	pdata_count = 0;

	if (rva_displacements_file != NULL)
	{
		fclose(rva_displacements_file);
		rva_displacements_file = NULL;
	}

	if (invalid_code_fixups_file != NULL)
	{
		fclose(invalid_code_fixups_file);
		invalid_code_fixups_file = NULL;
	}

	if (SR_CodeBase != NULL)
	{
		free(SR_CodeBase);
		SR_CodeBase = NULL;
	}

	SR_fh4_free();
	SR_elf_eh_free();
	SR_elf_free();
}

static void initialize_values(void)
{
	num_sections = 0;
	SR_CodeBase = NULL;
	InputFormat = INPUT_PE;
	CodeBase = 0;
	ElfPie = 0;
	ElfLoadBias = 0;
	ImageBase = 0;
	SizeOfImage = 0;
	SizeOfHeaders = 0;
	FirstSectionRVA = 0;
	pdata_list = NULL;
	pdata_count = 0;
	list_invalid_code_fixups = 0;
	invalid_code_fixups_name = NULL;
	invalid_code_fixups_file = NULL;
	list_data_to_code_fixups = 0;
	data_to_code_fixups_name = NULL;
	list_rva_displacements = 0;
	rva_displacements_name = NULL;
	rva_displacements_file = NULL;
	generate_relocations_name = NULL;
	add_tracing_to_code = 0;

	use_pdata = 1;
	convert_exception_data = 1;
	detect_jump_tables = 1;
	detect_image_relative = 1;
	keep_original_code = 1;

	ud_init(&ud_obj);
	ud_set_mode(&ud_obj, 64);
	// instructions of all vendors (default is AMD, which makes Intel only instructions like endbr64 invalid)
	ud_set_vendor(&ud_obj, UD_VENDOR_ANY);
	ud_set_syntax(&ud_obj, UD_SYN_INTEL);
}

static void print_usage(void)
{
	fprintf(stderr, "Usage: SRW64 [options] <input executable> <output.asm>\n");
	fprintf(stderr, "Input: 64-bit Windows executable (PE32+) or statically linked 64-bit Linux executable (ELF64)\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t--list_invalid_code_fixups=<file>  list code references to code which are not considered as code entries\n");
	fprintf(stderr, "\t--list_data_to_code_fixups=<file>  list data references to code\n");
	fprintf(stderr, "\t--list_rva_displacements=<file>    list instructions with converted image relative displacements\n");
	fprintf(stderr, "\t--generate_relocations=<file>      guess relocations for executable with stripped relocations\n");
	fprintf(stderr, "\t--add_tracing                      add TRACE_LABEL macro to labeled instructions\n");
}

int main (int argc, char *argv[])
{
	int ret;
	char *input_name, *output_name;

#ifdef _WIN32
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
	if (-1 == nice(1)) {}
#endif

#if (OUTPUT_TYPE == OUT_ORIG)
	fprintf(stderr, "Output type: Original\n");
#elif (OUTPUT_TYPE == OUT_X64)
	fprintf(stderr, "Output type: X64 version\n");
#else
	#error unknown output type
#endif


	initialize_values();

	read_cfg();

	fprintf(stderr, "use pdata: %s\n", (use_pdata)?"yes":"no");
	fprintf(stderr, "convert exception data: %s\n", (convert_exception_data)?"yes":"no");
	fprintf(stderr, "detect jump tables: %s\n", (detect_jump_tables)?"yes":"no");
	fprintf(stderr, "detect image relative addressing: %s\n", (detect_image_relative)?"yes":"no");

	input_name = NULL;
	output_name = NULL;

	argv++;
	argc--;
	while (argc)
	{
		if ((*argv)[0] == '-')
		{
			if (strncasecmp(*argv, "--list_invalid_code_fixups=", 27) == 0)
			{
				list_invalid_code_fixups = 1;
				invalid_code_fixups_name = &((*argv)[27]);
			}
			else if (strncasecmp(*argv, "--list_data_to_code_fixups=", 27) == 0)
			{
				list_data_to_code_fixups = 1;
				data_to_code_fixups_name = &((*argv)[27]);
			}
			else if (strncasecmp(*argv, "--list_rva_displacements=", 25) == 0)
			{
				list_rva_displacements = 1;
				rva_displacements_name = &((*argv)[25]);
			}
			else if (strncasecmp(*argv, "--generate_relocations=", 23) == 0)
			{
				generate_relocations_name = &((*argv)[23]);
			}
			else if (strcasecmp(*argv, "--add_tracing") == 0)
			{
				add_tracing_to_code = 1;
			}
			else
			{
				fprintf(stderr, "Error: unknown option: %s\n", *argv);
				print_usage();
				return 1;
			}
		}
		else
		{
			if (input_name == NULL) input_name = *argv;
			else if (output_name == NULL) output_name = *argv;
		}
		argv++;
		argc--;
	} ;

	if (input_name == NULL || output_name == NULL)
	{
		print_usage();
		return 1;
	}

	if (list_invalid_code_fixups) list_data_to_code_fixups = 0;

	if (list_rva_displacements)
	{
		rva_displacements_file = fopen(rva_displacements_name, "wt");
		if (rva_displacements_file == NULL)
		{
			fprintf(stderr, "Error: unable to create file: %s\n", rva_displacements_name);
			list_rva_displacements = 0;
		}
	}

	write_log_time(stderr);
	fprintf(stderr, "Loading EXE file...\n");

	ret = SRW64_LoadFile(input_name);
	if (ret != 0)
	{
		fprintf(stderr, "Error: %i\n", ret);
		deinitialize_values();
		return 2;
	}

	fprintf(stderr, "\tSections in file: %i\n", num_sections);

#if (OUTPUT_TYPE != OUT_ORIG)
	write_log_time(stderr);
	fprintf(stderr, "Loading source code information...\n");

	ret = SR_LoadSCI();
	if (ret != 0)
	{
		fprintf(stderr, "Error: %i\n", ret);
		deinitialize_values();
		return 3;
	}
#endif

	write_log_time(stderr);
	fprintf(stderr, "Initial disassembly...\n");
	if ( SR_initial_disassembly() != 0 )
	{
		deinitialize_values();
		return 4;
	}

#if (OUTPUT_TYPE != OUT_ORIG)
	if (InputFormat == INPUT_ELF)
	{
		write_log_time(stderr);
		fprintf(stderr, "Processing symbols and data...\n");
		ret = SR_elf_process();
		if (ret != 0)
		{
			fprintf(stderr, "Error: %i\n", ret);
			deinitialize_values();
			return 4;
		}
	}

	write_log_time(stderr);
	fprintf(stderr, "Processing exception directory...\n");
	ret = SR_process_pdata();
	if (ret != 0)
	{
		fprintf(stderr, "Error: %i\n", ret);
		deinitialize_values();
		return 4;
	}

	ret = SR_process_delay_imports();
	if (ret != 0)
	{
		fprintf(stderr, "Error: %i\n", ret);
		deinitialize_values();
		return 4;
	}

	if (detect_image_relative && InputFormat == INPUT_PE)
	{
		ret = SR_prescan_image_base();
		if (ret != 0)
		{
			fprintf(stderr, "Error: %i\n", ret);
			deinitialize_values();
			return 4;
		}
	}
#endif

	write_log_time(stderr);
	fprintf(stderr, "Applying fixup...\n");
	if ( SR_apply_fixup_info() != 0 )
	{
		deinitialize_values();
		return 5;
	}

#if (OUTPUT_TYPE != OUT_ORIG)
	write_log_time(stderr);
	fprintf(stderr, "Full disassembly...\n");
	ret = SR_full_disassembly();
	if (ret != 0)
	{
		fprintf(stderr, "Error: %i\n", ret);
		deinitialize_values();
		return 6;
	}

	if (convert_exception_data)
	{
		ret = SR_process_unwind_offsets();
		if (ret != 0)
		{
			fprintf(stderr, "Error: %i\n", ret);
			deinitialize_values();
			return 6;
		}

		ret = SR_fh4_post_process();
		if (ret != 0)
		{
			fprintf(stderr, "Error: %i\n", ret);
			deinitialize_values();
			return 6;
		}

		// Linux executables: new .eh_frame and LSDAs for recompiled code
		ret = SR_elf_eh_post_process();
		if (ret != 0)
		{
			fprintf(stderr, "Error: %i\n", ret);
			deinitialize_values();
			return 6;
		}
	}
#endif

	write_log_time(stderr);
	fprintf(stderr, "Writing output...\n");

	ret = SR_write_output(output_name);

	if (ret != 0)
	{
		fprintf(stderr, "Error: %i\n", ret);
		deinitialize_values();
		return 7;
	}

	write_log_time(stderr);
	fprintf(stderr, "Finishing...\n");

	deinitialize_values();

	return 0;
}
