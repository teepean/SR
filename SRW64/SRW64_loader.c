/**
 *
 *  Copyright (C) 2019-2025 Roman Pauer
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
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"

#if (defined(__WIN32__) || defined(__WINDOWS__)) && !defined(_WIN32)
#define _WIN32
#endif

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN

    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

#define _DEBUG_PE 1

#pragma pack(1)

typedef struct PACKED __IMAGE_FILE_HEADER {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} _IMAGE_FILE_HEADER, *P_IMAGE_FILE_HEADER;

typedef struct PACKED __IMAGE_DATA_DIRECTORY {
    uint32_t VirtualAddress;
    uint32_t Size;
} _IMAGE_DATA_DIRECTORY, *P_IMAGE_DATA_DIRECTORY;

typedef struct PACKED __IMAGE_OPTIONAL_HEADER64 {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    _IMAGE_DATA_DIRECTORY DataDirectory[16];
} _IMAGE_OPTIONAL_HEADER64, *P_IMAGE_OPTIONAL_HEADER64;


typedef struct PACKED __IMAGE_NT_HEADERS64 {
    uint32_t Signature;
    struct __IMAGE_FILE_HEADER FileHeader;
    struct __IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} _IMAGE_NT_HEADERS64, *P_IMAGE_NT_HEADERS64;

typedef struct PACKED __IMAGE_SECTION_HEADER {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} _IMAGE_SECTION_HEADER, *P_IMAGE_SECTION_HEADER;

typedef struct PACKED __IMAGE_IMPORT_DESCRIPTOR {
    uint32_t ImportLookupTable;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t ImportAddressTable;
} _IMAGE_IMPORT_DESCRIPTOR, *P_IMAGE_IMPORT_DESCRIPTOR;

typedef struct PACKED __IMAGE_EXPORT_DIRECTORY {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Name;
    uint32_t Base;
    uint32_t NumberOfFunctions;
    uint32_t NumberOfNames;
    uint32_t AddressOfFunctions;
    uint32_t AddressOfNames;
    uint32_t AddressOfNameOrdinals;
} _IMAGE_EXPORT_DIRECTORY, *P_IMAGE_EXPORT_DIRECTORY;

#pragma pack()

#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b

#define DIR_EXPORT      0
#define DIR_IMPORT      1
#define DIR_RESOURCE    2
#define DIR_EXCEPTION   3
#define DIR_BASERELOC   5
#define DIR_TLS         9
#define DIR_IAT         12
#define DIR_DELAYIMPORT 13
#define DIR_CLR         14

typedef struct _Import_Check_ {
    import_data *result;
    const char *name;
} Import_Check;

typedef struct _orig_section_ {
    uint_fast32_t virtaddr;
    uint_fast32_t size;
    uint8_t *adr;
} orig_section;

static orig_section orig_sections[MAX_SECTIONS];
static unsigned int num_orig_sections = 0;


typedef struct _memory_file_ {
    int64_t len;
    void *mem;
#ifdef _WIN32
    HANDLE file, fmap;
#else
    int fd;
#endif
    int mem_allocated;
} memory_file;

static int load_file(memory_file *mem_file, const char *fname)
{
    FILE *mf;

    if (mem_file == NULL)
    {
        return -1;
    }

    mem_file->mem_allocated = 0;
    mem_file->mem = NULL;
#ifdef _WIN32

    mem_file->fmap = INVALID_HANDLE_VALUE;

    mem_file->file = CreateFile(fname, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (mem_file->file == INVALID_HANDLE_VALUE) goto LOAD_FILE_MAP_ERROR;

    {
        DWORD loword, hiword;

        loword = GetFileSize(mem_file->file, &hiword);
        if (loword == INVALID_FILE_SIZE) goto LOAD_FILE_MAP_ERROR;

        mem_file->len = loword | ( ((int64_t) hiword) << 32 );
    }

    if (mem_file->len > (1LL << ( (sizeof(void *) * 8) - 2) ) ) goto LOAD_FILE_MAP_ERROR;

    mem_file->fmap = CreateFileMapping(mem_file->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (mem_file->fmap == NULL) goto LOAD_FILE_MAP_ERROR;

    mem_file->mem = MapViewOfFile(mem_file->fmap, FILE_MAP_READ, 0, 0, 0);
    if (mem_file->mem == NULL) goto LOAD_FILE_MAP_ERROR;

    return 0;

LOAD_FILE_MAP_ERROR:
    if (mem_file->fmap != INVALID_HANDLE_VALUE && mem_file->fmap != NULL)
    {
        CloseHandle(mem_file->fmap);
    }
    mem_file->fmap = INVALID_HANDLE_VALUE;

    if (mem_file->file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(mem_file->file);
        mem_file->file = INVALID_HANDLE_VALUE;
    }
#else
    mem_file->fd = open(fname, O_RDONLY);
    if (mem_file->fd < 0) goto LOAD_FILE_MAP_ERROR;

    {
        off_t len2;

        len2 = lseek(mem_file->fd, 0, SEEK_END);

        if (len2 < 0) goto LOAD_FILE_MAP_ERROR;

        if (lseek(mem_file->fd, 0, SEEK_SET) == (off_t)-1) goto LOAD_FILE_MAP_ERROR;

        mem_file->len = (int64_t) len2;
    }

    mem_file->mem = mmap(NULL, mem_file->len, PROT_READ, MAP_PRIVATE, mem_file->fd, 0);
    if(mem_file->mem == MAP_FAILED) goto LOAD_FILE_MAP_ERROR;

    return 0;

    LOAD_FILE_MAP_ERROR:
    mem_file->mem = NULL;

    if (mem_file->fd >= 0)
    {
        close(mem_file->fd);
    }
    mem_file->fd = -1;
#endif

    mf = fopen(fname, "rb");
    if (mf == NULL) goto LOAD_FILE_READ_ERROR;

    {
        long len2;

        if (fseek(mf, 0, SEEK_END) == -1) goto LOAD_FILE_READ_ERROR;

        len2 = ftell(mf);

        if (len2 < 0) goto LOAD_FILE_READ_ERROR;

        if (fseek(mf, 0, SEEK_SET) == -1) goto LOAD_FILE_READ_ERROR;

        mem_file->len = (int64_t) len2;
    }

    mem_file->mem = malloc((size_t)mem_file->len);
    if (mem_file->mem == NULL) goto LOAD_FILE_READ_ERROR;

    mem_file->mem_allocated = 1;

    if (fread(mem_file->mem, 1, (size_t)mem_file->len, mf) != (size_t)mem_file->len) goto LOAD_FILE_READ_ERROR;

    fclose(mf);

    return 0;

LOAD_FILE_READ_ERROR:
    if (mem_file->mem_allocated)
    {
        mem_file->mem_allocated = 0;
        free(mem_file->mem);
        mem_file->mem = NULL;
    }

    if (mf != NULL)
    {
        fclose(mf);
    }

    return -2;
}

static void unload_file(memory_file *mem_file)
{
    if (mem_file == NULL) return;

    if (mem_file->mem_allocated)
    {
        mem_file->mem_allocated = 0;
        if (mem_file->mem != NULL)
        {
            free(mem_file->mem);
            mem_file->mem = NULL;
        }
    }
#ifdef _WIN32
    else
    {
        if (mem_file->mem != NULL)
        {
            UnmapViewOfFile(mem_file->mem);
            mem_file->mem = NULL;
        }

        if (mem_file->fmap != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mem_file->fmap);
            mem_file->fmap = INVALID_HANDLE_VALUE;
        }

        if (mem_file->file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mem_file->file);
            mem_file->file = INVALID_HANDLE_VALUE;
        }
    }
#else
    else
    {
        if (mem_file->mem != NULL)
        {
            munmap(mem_file->mem, mem_file->len);
            mem_file->mem = NULL;
        }

        if (mem_file->fd >= 0)
        {
            close(mem_file->fd);
            mem_file->fd = -1;
        }
    }
#endif
}


void *SR_rva_to_ptr(uint_fast32_t rva, uint_fast32_t size)
{
    unsigned int Entry;

    for (Entry = 0; Entry < num_orig_sections; Entry++)
    {
        if ((rva >= orig_sections[Entry].virtaddr) &&
            ((uint64_t)rva + size <= (uint64_t)orig_sections[Entry].virtaddr + orig_sections[Entry].size)
           )
        {
            return &(orig_sections[Entry].adr[rva - orig_sections[Entry].virtaddr]);
        }
    }

    return NULL;
}

static const char *rva_to_string(uint_fast32_t rva)
{
    unsigned int Entry;
    const char *str;
    uint_fast32_t maxlen, len;

    for (Entry = 0; Entry < num_orig_sections; Entry++)
    {
        if ((rva >= orig_sections[Entry].virtaddr) &&
            (rva < orig_sections[Entry].virtaddr + orig_sections[Entry].size)
           )
        {
            str = (const char *) &(orig_sections[Entry].adr[rva - orig_sections[Entry].virtaddr]);
            maxlen = orig_sections[Entry].virtaddr + orig_sections[Entry].size - rva;
            for (len = 0; len < maxlen; len++)
            {
                if (str[len] == 0) return str;
            }
            return NULL;
        }
    }

    return NULL;
}


static void ImportNameCheck(import_data *item, void *data)
{
#define DATA ((Import_Check *) data)

    if ( !(DATA->result) )
    {
        if ( !strcmp(item->ProcName, DATA->name) )
        {
            DATA->result = item;
        }
    }

#undef DATA
}

static import_data *FindImportName(const char *ImportName)
{
    Import_Check IC;

    IC.result = NULL;
    IC.name = ImportName;
    import_list_ForEach(&ImportNameCheck, &IC);

    return IC.result;
}

static uint_fast32_t lowest_bit_alignment(uint_fast32_t value, uint_fast32_t max_align)
{
    uint_fast32_t align;

    align = 1;
    while ((align < max_align) && ((value & align) == 0))
    {
        align <<= 1;
    }

    return align;
}

// splits section Entry at relative offset RelOfs, returns number of new section (containing the second part)
static int split_section(unsigned int Entry, uint_fast32_t RelOfs)
{
    unsigned int NewEntry;

    if (num_sections >= MAX_SECTIONS)
    {
        fprintf(stderr, "Error: too many sections\n");
        return -1;
    }

    NewEntry = num_sections;
    num_sections++;

    section[NewEntry] = section[Entry];
    section[NewEntry].size -= RelOfs;
    section[NewEntry].size_in_file = (section[Entry].size_in_file > RelOfs)?(section[Entry].size_in_file - RelOfs):0;
    section[NewEntry].start += RelOfs;
    section[NewEntry].virtaddr += RelOfs;
    section[NewEntry].adr += RelOfs;
    section[NewEntry].align = lowest_bit_alignment(section[NewEntry].virtaddr, 4096);

    section[Entry].size = RelOfs;
    if (section[Entry].size_in_file > RelOfs)
    {
        section[Entry].size_in_file = RelOfs;
    }

    return (int)NewEntry;
}

// moves address range into separate section with given type
static int carve_section_range(uint_fast32_t rva, uint_fast32_t size, section_type type, const char *description)
{
    unsigned int Entry;
    int NewEntry;
    uint_fast32_t RelOfs;

    if (rva == 0 || size == 0) return 0;

    for (Entry = 0; Entry < num_sections; Entry++)
    {
        if (rva >= section[Entry].virtaddr && rva < section[Entry].virtaddr + section[Entry].size) break;
    }

    if (Entry >= num_sections)
    {
        fprintf(stderr, "Warning: %s is not inside any section: 0x%x\n", description, (unsigned int)rva);
        return 0;
    }

    if ((section[Entry].type != ST_DATA) && (section[Entry].type != ST_RDATA))
    {
        if (section[Entry].type != type)
        {
            fprintf(stderr, "Warning: %s is inside section of type %i: %s\n", description, section[Entry].type, section[Entry].name);
        }
        return 0;
    }

    RelOfs = rva - section[Entry].virtaddr;
    if (RelOfs + size > section[Entry].size)
    {
        size = section[Entry].size - RelOfs;
    }

    if (RelOfs != 0)
    {
        NewEntry = split_section(Entry, RelOfs);
        if (NewEntry < 0) return -1;
        Entry = (unsigned int)NewEntry;
    }

    if (size < section[Entry].size)
    {
        if (split_section(Entry, size) < 0) return -1;
    }

#ifdef _DEBUG_PE
    fprintf(stderr, "%s moved to separate section: %s (seg%.2i) - 0x%x - %i - type %i\n", description, section[Entry].name, Entry + 1, (unsigned int)section[Entry].virtaddr, (unsigned int)section[Entry].size, type);
#endif

    section[Entry].type = type;

    return 0;
}

static int LoadBssBorder(void)
{
    char buf[8192];
    char *str1;
    FILE *file;
    uint_fast32_t BssObject, BssOffset;
    size_t length;
    int items, BssAlignMinus, NewEntry;
    uint64_t BssAddress;

    file = fopen("bssborder.csv", "rt");
    if (file == NULL) return 0;

    while (!feof(file))
    {
        // read enters
        items = fscanf(file, "%8191[\n]", buf);
        // read line
        buf[0] = 0;
        items = fscanf(file, "%8191[^\n]", buf);
        if (items <= 0) continue;
        length = strlen(buf);
        if (length != 0 && buf[length - 1] == '\r')
        {
            length--;
            buf[length] = 0;
        }

        if (length == 0) continue;

        str1 = strchr(buf, ',');

        if (str1 != NULL)
        {
            *str1 = 0;
            str1++;

            BssAlignMinus = (int)strtol(str1, NULL, 0);
        }
        else
        {
            BssAlignMinus = 0;
        }

        BssAddress = strtoull(buf, NULL, 0);

        if (!SR_get_section_reladr_exact(BssAddress, &BssObject, &BssOffset))
        {
            fprintf(stderr, "Error: wrong bss address: 0x%" PRIx64 "\n", BssAddress);
            fclose(file);
            return 1;
        }

        if (section[BssObject].type != ST_DATA)
        {
            fprintf(stderr, "Error: wrong bss section type: 0x%" PRIx64 " - %i\n", BssAddress, section[BssObject].type);
            fclose(file);
            return 2;
        }

        NewEntry = split_section(BssObject, BssOffset);
        if (NewEntry < 0)
        {
            fclose(file);
            return 3;
        }

        section[NewEntry].size_in_file = 0;
        section[NewEntry].type = ST_UDATA;
        strcpy(section[NewEntry].name, ".bss");
#ifdef _DEBUG_PE
        fprintf(stderr, "splitting bss section from end of %s (seg%.2i) to %s (seg%.2i)\n", section[BssObject].name, (unsigned int)(BssObject + 1), section[NewEntry].name, NewEntry + 1);
        fprintf(stderr, "\tnew section start=0x%" PRIx64 " and size=%i and type=%i\n", section[NewEntry].start, (unsigned int)section[NewEntry].size, section[NewEntry].type);
#endif

        section[BssObject].size = BssOffset + BssAlignMinus;
        if (section[BssObject].size_in_file > BssOffset + BssAlignMinus)
        {
            section[BssObject].size_in_file = BssOffset + BssAlignMinus;
        }
    }

    fclose(file);

    return 0;
}

static int AddRelocation(uint64_t FixupAddress, fixup_type FixupType, int check_value, uint64_t TargetAddress)
{
    uint_fast32_t SourceObject, SourceOffset, TargetObject, TargetOffset;
    uint64_t StoredValue;
    fixup_data *fixup;

    if (!SR_get_section_reladr_exact(FixupAddress, &SourceObject, &SourceOffset))
    {
        fprintf(stderr, "Warning: relocation outside of sections: 0x%" PRIx64 "\n", FixupAddress);
        return 0;
    }

    if (SourceOffset + FIXUP_SIZE(FixupType) > section[SourceObject].size)
    {
        fprintf(stderr, "Warning: relocation crosses section end: 0x%" PRIx64 "\n", FixupAddress);
        return 0;
    }

    switch (FixupType)
    {
        case FT_DIR64:
            memcpy(&StoredValue, &(section[SourceObject].adr[SourceOffset]), 8);
            break;
        case FT_RVA32:
            {
                uint32_t value32;
                memcpy(&value32, &(section[SourceObject].adr[SourceOffset]), 4);
                StoredValue = ImageBase + value32;
            }
            break;
        default:
            {
                uint32_t value32;
                memcpy(&value32, &(section[SourceObject].adr[SourceOffset]), 4);
                StoredValue = value32;
            }
            break;
    }

    if (check_value)
    {
        if (TargetAddress != StoredValue)
        {
            fprintf(stderr, "Error: inconsistent relocation target address: 0x%" PRIx64 " - 0x%" PRIx64 "\n", FixupAddress, TargetAddress);
            return 2;
        }
    }
    else
    {
        TargetAddress = StoredValue;
    }

    if (TargetAddress >= ImageBase && TargetAddress < ImageBase + SizeOfHeaders)
    {
        TargetObject = SEC_HEADER;
        TargetOffset = (uint_fast32_t)(TargetAddress - ImageBase);
    }
    else if (!SR_get_section_reladr(TargetAddress, &TargetObject, &TargetOffset))
    {
        fprintf(stderr, "Warning: relocation target outside of image: 0x%" PRIx64 " - 0x%" PRIx64 "\n", FixupAddress, TargetAddress);
        return 0;
    }

    fixup = section_fixup_list_FindEntryEqual(SourceObject, SourceOffset);
    if (fixup == NULL)
    {
        fixup = section_fixup_list_Insert(SourceObject, SourceOffset);
        if (fixup == NULL)
        {
            fprintf(stderr, "Error: error adding relocation: 0x%" PRIx64 "\n", FixupAddress);
            return 5;
        }

        fixup->tofs = TargetOffset;
        fixup->tsec = TargetObject;
        fixup->type = FixupType;
        fixup->flags = 0;
    }
    else
    {
        if (fixup->tofs != (int_fast32_t)TargetOffset ||
            fixup->tsec != TargetObject ||
            fixup->type != FixupType)
        {
            fprintf(stderr, "Error: error adding relocation twice: 0x%" PRIx64 "\n", FixupAddress);
            return 4;
        }
    }

    return 0;
}

static int LoadRelocations(void)
{
    char buf[8192];
    char *str1, *str2;
    FILE *file;
    size_t length;
    int items, ret;
    uint64_t FixupAddress, TargetAddress;
    fixup_type FixupType;

    file = fopen("relocations.csv", "rt");
    if (file == NULL)
    {
        return -1;
    }

    while (!feof(file))
    {
        // read enters
        items = fscanf(file, "%8191[\n]", buf);
        // read line
        buf[0] = 0;
        items = fscanf(file, "%8191[^\n]", buf);
        if (items <= 0) continue;
        length = strlen(buf);
        if (length != 0 && buf[length - 1] == '\r')
        {
            length--;
            buf[length] = 0;
        }

        if (length == 0) continue;
        if (buf[0] == '#' || buf[0] == ';') continue;

        str1 = strchr(buf, ',');

        if (str1 == NULL) continue;
        *str1 = 0;
        str1++;

        str2 = strchr(str1, ',');
        if (str2 != NULL)
        {
            *str2 = 0;
            str2++;

            while (*str2 == ' ') str2++;
            if (*str2 == 0) str2 = NULL;
        }

        FixupAddress = strtoull(buf, NULL, 0);
        TargetAddress = strtoull(str1, NULL, 0);

        // type: (empty) or 'q' - 64-bit address, 'd' - 32-bit address, 'r' - 32-bit rva
        FixupType = FT_DIR64;
        if (str2 != NULL)
        {
            if (*str2 == 'd' || *str2 == 'D') FixupType = FT_NORMAL;
            else if (*str2 == 'r' || *str2 == 'R') FixupType = FT_RVA32;
        }

        if (FixupType == FT_RVA32 && TargetAddress < ImageBase)
        {
            TargetAddress += ImageBase;
        }

        ret = AddRelocation(FixupAddress, FixupType, 1, TargetAddress);
        if (ret)
        {
            fclose(file);
            return ret;
        }
    }

    fclose(file);

    return 0;
}

// guess relocations in executable with stripped relocations - every aligned 64-bit value pointing inside the image is considered a relocation
static int GenerateRelocations(const char *fname)
{
    FILE *file;
    unsigned int Entry;
    uint_fast32_t ofs;
    uint64_t value;
    int ret, count;

    file = fopen(fname, "wt");
    if (file == NULL)
    {
        fprintf(stderr, "Error: unable to create file: %s\n", fname);
        return -1;
    }

    count = 0;
    for (Entry = 0; Entry < num_sections; Entry++)
    {
        if ((section[Entry].type == ST_UDATA) ||
            (section[Entry].type == ST_RSRC) ||
            (section[Entry].type == ST_RELOC) ||
            (section[Entry].type == ST_IDATA) ||
            (section[Entry].type == ST_EDATA)
           )
        {
            continue;
        }

        for (ofs = (uint_fast32_t)((8 - (section[Entry].start & 7)) & 7); ofs + 8 <= section[Entry].size_in_file; ofs += 8)
        {
            memcpy(&value, &(section[Entry].adr[ofs]), 8);

            if (value >= ImageBase + FirstSectionRVA && value <= ImageBase + SizeOfImage)
            {
                fprintf(file, "0x%" PRIx64 ",0x%" PRIx64 ",\n", section[Entry].start + ofs, value);

                ret = AddRelocation(section[Entry].start + ofs, FT_DIR64, 1, value);
                if (ret)
                {
                    fclose(file);
                    return ret;
                }

                count++;
            }
        }
    }

    fclose(file);

    fprintf(stderr, "Generated %i relocations - check the generated file: %s\n", count, fname);

    return 0;
}

static int LoadBaseRelocations(uint_fast32_t RelocRVA, uint_fast32_t RelocSize)
{
    uint8_t *Fixups;
    uint_fast32_t FixupOffset, PageRVA, BlockSize, BlockOffset, FixupField, FixupType;
    int ret;

    Fixups = (uint8_t *) SR_rva_to_ptr(RelocRVA, RelocSize);
    if (Fixups == NULL)
    {
        fprintf(stderr, "Error: base relocation table is not inside any section\n");
        return -1;
    }

    FixupOffset = 0;
    while (FixupOffset + 8 <= RelocSize)
    {
        PageRVA = *((uint32_t *) &(Fixups[FixupOffset]));
        BlockSize = *((uint32_t *) &(Fixups[FixupOffset + 4]));

        if (BlockSize < 8 || FixupOffset + BlockSize > RelocSize)
        {
            if (BlockSize != 0)
            {
                fprintf(stderr, "Warning: invalid base relocation block size: %i\n", (unsigned int)BlockSize);
            }
            break;
        }

        for (BlockOffset = 8; BlockOffset + 2 <= BlockSize; BlockOffset += 2)
        {
            FixupField = *((uint16_t *) &(Fixups[FixupOffset + BlockOffset]));
            FixupType = (FixupField & 0xf000) >> 12;

            switch (FixupType)
            {
                case 0: // IMAGE_REL_BASED_ABSOLUTE
                    break;
                case 3: // IMAGE_REL_BASED_HIGHLOW
                    ret = AddRelocation(ImageBase + PageRVA + (FixupField & 0x0fff), FT_NORMAL, 0, 0);
                    if (ret) return ret;
                    NumAbsolute32Relocations++;
                    break;
                case 10: // IMAGE_REL_BASED_DIR64
                    ret = AddRelocation(ImageBase + PageRVA + (FixupField & 0x0fff), FT_DIR64, 0, 0);
                    if (ret) return ret;
                    break;
                default:
                    fprintf(stderr, "Error: Unknown fixup type: %i\n", (unsigned int)FixupType);
                    return -1;
            }
        }

        FixupOffset += BlockSize;
    }

    return 0;
}

static const char *ImportOrdinalName(const char *DllName, uint_fast32_t Ordinal, int *generated)
{
    char *name;
    size_t len;
    unsigned int index;

    *generated = 0;

    if ((Ordinal == 17) && (strcasecmp(DllName, "comctl32.dll") == 0))
    {
        return "InitCommonControls";
    }

    if (strcasecmp(DllName, "dsound.dll") == 0)
    {
        switch (Ordinal)
        {
            case 1: return "DirectSoundCreate";
            case 2: return "DirectSoundEnumerateA";
            case 3: return "DirectSoundEnumerateW";
            case 11: return "DirectSoundCreate8";
        }
    }

    if ((strcasecmp(DllName, "ws2_32.dll") == 0) || (strcasecmp(DllName, "wsock32.dll") == 0))
    {
        switch (Ordinal)
        {
            case   1: return "accept";
            case   2: return "bind";
            case   3: return "closesocket";
            case   4: return "connect";
            case   5: return "getpeername";
            case   6: return "getsockname";
            case   7: return "getsockopt";
            case   8: return "htonl";
            case   9: return "htons";
            case  10: return "ioctlsocket";
            case  11: return "inet_addr";
            case  12: return "inet_ntoa";
            case  13: return "listen";
            case  14: return "ntohl";
            case  15: return "ntohs";
            case  16: return "recv";
            case  17: return "recvfrom";
            case  18: return "select";
            case  19: return "send";
            case  20: return "sendto";
            case  21: return "setsockopt";
            case  22: return "shutdown";
            case  23: return "socket";
            case  51: return "gethostbyaddr";
            case  52: return "gethostbyname";
            case  57: return "gethostname";
            case 111: return "WSAGetLastError";
            case 115: return "WSAStartup";
            case 116: return "WSACleanup";
        }
    }

    if (strcasecmp(DllName, "oleaut32.dll") == 0)
    {
        switch (Ordinal)
        {
            case 2: return "SysAllocString";
            case 4: return "SysAllocStringLen";
            case 6: return "SysFreeString";
            case 7: return "SysStringLen";
            case 8: return "VariantInit";
            case 9: return "VariantClear";
        }
    }

    // generic name: dllname_ordinal_number
    len = strlen(DllName);
    name = (char *) malloc(len + 32);
    if (name == NULL) return NULL;

    for (index = 0; index < len; index++)
    {
        if (DllName[index] == '.') break;
        name[index] = ((DllName[index] >= 'A' && DllName[index] <= 'Z') || (DllName[index] >= 'a' && DllName[index] <= 'z') || (DllName[index] >= '0' && DllName[index] <= '9'))?DllName[index]:'_';
    }
    sprintf(&(name[index]), "_ordinal_%u", (unsigned int)Ordinal);

    fprintf(stderr, "Warning: import by ordinal: %s - %u - using name: %s\n", DllName, (unsigned int)Ordinal, name);

    *generated = 1;

    return name;
}

static int LoadImports(uint_fast32_t ImportRVA)
{
    P_IMAGE_IMPORT_DESCRIPTOR Directory;
    uint64_t *LookupTable;
    const char *DllName, *ProcName;
    char *ProcAdrName;
    extrn_data *extrn;
    import_data *import;
    uint_fast32_t ImportObject, ImportAddress, LookupRVA, Ordinal;
    uint32_t ImportNum;
    int generated;

    ImportNum = 0;

    for (;; ImportRVA += sizeof(_IMAGE_IMPORT_DESCRIPTOR))
    {
        Directory = (P_IMAGE_IMPORT_DESCRIPTOR) SR_rva_to_ptr(ImportRVA, sizeof(_IMAGE_IMPORT_DESCRIPTOR));
        if (Directory == NULL)
        {
            fprintf(stderr, "Error: import directory is not inside any section\n");
            return -1;
        }

        if (Directory->ImportAddressTable == 0 && Directory->Name == 0) break;

        DllName = rva_to_string(Directory->Name);
        if (DllName == NULL)
        {
            fprintf(stderr, "Error: invalid import dll name\n");
            return -1;
        }

        fprintf(stderr, "\t\tImported DLL: %s\n", DllName);

        LookupRVA = (Directory->ImportLookupTable != 0)?Directory->ImportLookupTable:Directory->ImportAddressTable;

        if (!SR_get_section_reladr_exact(ImageBase + Directory->ImportAddressTable, &ImportObject, &ImportAddress))
        {
            fprintf(stderr, "Error: import address table is not inside any section\n");
            return -1;
        }

        for (;; LookupRVA += 8, ImportAddress += 8)
        {
            LookupTable = (uint64_t *) SR_rva_to_ptr(LookupRVA, 8);
            if (LookupTable == NULL)
            {
                fprintf(stderr, "Error: import lookup table is not inside any section\n");
                return -1;
            }

            if (*LookupTable == 0) break;

            Ordinal = 0;
            if (*LookupTable & UINT64_C(0x8000000000000000))
            {
                ProcName = ImportOrdinalName(DllName, (uint_fast32_t)(*LookupTable & 0xffff), &generated);
                if (generated) Ordinal = (uint_fast32_t)(*LookupTable & 0xffff);
                if (ProcName == NULL)
                {
                    fprintf(stderr, "Error: Import by ordinal not supported\n");
                    return -1;
                }
            }
            else
            {
                ProcName = rva_to_string((uint_fast32_t)(*LookupTable & 0x7fffffff) + 2);
                if (ProcName == NULL)
                {
                    fprintf(stderr, "Error: invalid import name\n");
                    return -1;
                }
            }

#if (defined(_DEBUG_PE) && (_DEBUG_PE >= 2))
            fprintf(stderr, "%s - %i - 0x%x\n", ProcName, (unsigned int)ImportObject, (unsigned int)ImportAddress);
#endif

            ProcAdrName = (char *) malloc(strlen(ProcName) + 6);
            if (ProcAdrName == NULL)
            {
                fprintf(stderr, "Error: error allocation import memory - %i\n", (unsigned int)ImportAddress);
                return -4;
            }
            strcpy(ProcAdrName, "_adr_");
            strcat(ProcAdrName, ProcName);

            import = FindImportName(ProcName);
            if (import)
            {
                free(ProcAdrName);

                if (0 != strcasecmp(import->DllName, DllName))
                {
                    fprintf(stderr, "Error: import name conflict - %s - %s - %s\n", ProcName, import->DllName, DllName);
                    return -2;
                }
            }
            else
            {
                import = import_list_Insert(ImportNum);
                if ( import == NULL )
                {
                    free(ProcAdrName);
                    fprintf(stderr, "Error: error adding import - %i\n", ImportNum);
                    return -2;
                }
                import->DllName = DllName;
                import->ProcAdrName = ProcAdrName;
                import->ProcName = ProcName;
                import->Ordinal = Ordinal;
            }

            ImportNum++;


            extrn = section_extrn_list_FindEntryEqual(ImportObject, ImportAddress);
            if (extrn == NULL)
            {
                extrn = section_extrn_list_Insert(ImportObject, ImportAddress, ProcName, NULL);
                if (extrn == NULL)
                {
                    fprintf(stderr, "Error: error adding import extrn - %i\n", (unsigned int)ImportAddress);
                    return -2;
                }

                extrn->is_import = 1;
            }
            else
            {
                if ( strcmp(extrn->proc, ProcName) )
                {
                    fprintf(stderr, "Error: error adding import extrn twice - %i\n", (unsigned int)ImportAddress);
                    return -3;
                }
            }
        }
    }

    return 0;
}

static int LoadExports(uint_fast32_t ExportRVA, uint_fast32_t ExportSize)
{
    P_IMAGE_EXPORT_DIRECTORY Directory;
    uint_fast32_t Entry, RelAdr, index;
    uint32_t *AddressTable;
    export_data *export;

    Directory = (P_IMAGE_EXPORT_DIRECTORY) SR_rva_to_ptr(ExportRVA, sizeof(_IMAGE_EXPORT_DIRECTORY));
    if (Directory == NULL)
    {
        fprintf(stderr, "Error: export directory is not inside any section\n");
        return -1;
    }

    AddressTable = (uint32_t *) SR_rva_to_ptr(Directory->AddressOfFunctions, 4 * Directory->NumberOfFunctions);
    if (AddressTable == NULL)
    {
        fprintf(stderr, "Error: export address table is not inside any section\n");
        return -1;
    }

#ifdef _DEBUG_PE
    fprintf(stderr, "Export Ordinal Base: %i\n", Directory->Base);
#endif

    // exports by name
    if (Directory->NumberOfNames)
    {
        uint32_t *NameTable;
        uint16_t *OrdinalTable;
        const char *ExportName;

        NameTable = (uint32_t *) SR_rva_to_ptr(Directory->AddressOfNames, 4 * Directory->NumberOfNames);
        OrdinalTable = (uint16_t *) SR_rva_to_ptr(Directory->AddressOfNameOrdinals, 2 * Directory->NumberOfNames);

        if (NameTable == NULL || OrdinalTable == NULL)
        {
            fprintf(stderr, "Error: export name table is not inside any section\n");
            return -1;
        }

        for (index = 0; index < Directory->NumberOfNames; index++)
        {
            ExportName = rva_to_string(NameTable[index]);
            if (ExportName == NULL || OrdinalTable[index] >= Directory->NumberOfFunctions) continue;

            // skip forwarders
            if (AddressTable[OrdinalTable[index]] >= ExportRVA && AddressTable[OrdinalTable[index]] < ExportRVA + ExportSize) continue;

            if (!SR_get_section_reladr_exact(ImageBase + AddressTable[OrdinalTable[index]], &Entry, &RelAdr)) continue;

            export = section_export_list_FindEntryEqual2(Entry, RelAdr, OrdinalTable[index] + Directory->Base);
            if (export == NULL)
            {
                export = section_export_list_Insert(Entry, RelAdr, OrdinalTable[index] + Directory->Base, ExportName);
                if (export == NULL)
                {
                    fprintf(stderr, "Error: error adding export - %s\n", ExportName);
                    return -2;
                }
            }
            else
            {
                if (strcmp(export->name, ExportName))
                {
                    fprintf(stderr, "Error: error adding export twice - %s\n", ExportName);
                    return -3;
                }
            }
        }
    }

    // exports by ordinal
    for (index = 0; index < Directory->NumberOfFunctions; index++)
    {
        if (AddressTable[index] == 0) continue;

        // skip forwarders
        if (AddressTable[index] >= ExportRVA && AddressTable[index] < ExportRVA + ExportSize) continue;

        if (!SR_get_section_reladr_exact(ImageBase + AddressTable[index], &Entry, &RelAdr)) continue;

        export = section_export_list_FindEntryEqual2(Entry, RelAdr, index + Directory->Base);
        if (export == NULL)
        {
            export = section_export_list_Insert(Entry, RelAdr, index + Directory->Base, NULL);
            if (export == NULL)
            {
                fprintf(stderr, "Error: error adding export - %i\n", (unsigned int)index);
                return -2;
            }
        }
    }

    return 0;
}

int SRW64_LoadFile(const char *fname)
{
    memory_file mf;
    int64_t SR_FileSize;
    P_IMAGE_NT_HEADERS64 Header;
    uint8_t *MZfile, *PEfile;
    P_IMAGE_SECTION_HEADER SectionHeader;
    _IMAGE_DATA_DIRECTORY DataDirectory[16];
    uint64_t SR_CodeSize;
    uint8_t *SR_CodeRun;
    uint_fast32_t SectionAlignment, HeaderOffset, EntryPointRVA;
    int relocations_stripped;


    // load file
    if ( load_file(&mf, fname) )
    {
        fprintf(stderr, "Error: Unable to load file: %s\n", fname);
        return -1;
    }

    MZfile = (uint8_t *) mf.mem;
    SR_FileSize = mf.len;

    // check if file is big enough to contain MZ EXE header
    if (SR_FileSize <= 0x40)
    {
        unload_file(&mf);
        fprintf(stderr, "Error: File too small\n");
        return -1;
    }

    // check MZ signature
    if (*((uint16_t *)MZfile) != 0x5a4d)
    {
        unload_file(&mf);
        fprintf(stderr, "Error: not an EXE file\n");
        return -1;
    }

    // get PE header offset
    HeaderOffset = *((uint32_t *)&(MZfile[0x3c]));

    // check if file is big enough to contain PE EXE header
    if (SR_FileSize <= (int64_t)(HeaderOffset + 4 + sizeof(_IMAGE_FILE_HEADER) + 2))
    {
        unload_file(&mf);
        fprintf(stderr, "Error: File too small\n");
        return -1;
    }

    PEfile = &(MZfile[HeaderOffset]);
    Header = (P_IMAGE_NT_HEADERS64) PEfile;

    // check PE signature
    if ( Header->Signature != 0x00004550 )
    {
        unload_file(&mf);
        fprintf(stderr, "Error: Invalid EXE file\n");
        return -1;
    }

    if (Header->FileHeader.Machine == 0x014c || Header->OptionalHeader.Magic == 0x10b)
    {
        unload_file(&mf);
        fprintf(stderr, "Error: 32-bit executable - use SRW instead of SRW64\n");
        return -1;
    }

    // check PE header
    if ( Header->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || // x86-64
         Header->FileHeader.SizeOfOptionalHeader < (sizeof(_IMAGE_OPTIONAL_HEADER64) - sizeof(DataDirectory)) || // optional header size
         Header->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || // PE32+ executable
         (Header->OptionalHeader.Subsystem != 2 && Header->OptionalHeader.Subsystem != 3) // Windows GUI/CUI subsystem
        )
    {
        unload_file(&mf);
        fprintf(stderr, "Error: Wrong PE header (only PE32+ x86-64 Windows GUI/CUI executables are supported)\n");
        return -1;
    }

    if (SR_FileSize < (int64_t)(HeaderOffset + 4 + sizeof(_IMAGE_FILE_HEADER) + Header->FileHeader.SizeOfOptionalHeader + Header->FileHeader.NumberOfSections * sizeof(_IMAGE_SECTION_HEADER)))
    {
        unload_file(&mf);
        fprintf(stderr, "Error: File too small\n");
        return -1;
    }

    // copy data directories
    memset(DataDirectory, 0, sizeof(DataDirectory));
    {
        uint_fast32_t num_dirs, max_dirs;

        num_dirs = Header->OptionalHeader.NumberOfRvaAndSizes;
        max_dirs = (Header->FileHeader.SizeOfOptionalHeader - (sizeof(_IMAGE_OPTIONAL_HEADER64) - sizeof(DataDirectory))) / sizeof(_IMAGE_DATA_DIRECTORY);
        if (num_dirs > max_dirs) num_dirs = max_dirs;
        if (num_dirs > 16) num_dirs = 16;

        memcpy(DataDirectory, Header->OptionalHeader.DataDirectory, num_dirs * sizeof(_IMAGE_DATA_DIRECTORY));
    }

    if (DataDirectory[DIR_CLR].VirtualAddress != 0 && DataDirectory[DIR_CLR].Size != 0)
    {
        unload_file(&mf);
        fprintf(stderr, "Error: .NET executables are not supported\n");
        return -1;
    }

    ImageBase = Header->OptionalHeader.ImageBase;
    EntryPointRVA = Header->OptionalHeader.AddressOfEntryPoint;
    LargeAddressAware = (Header->FileHeader.Characteristics & 0x0020)?1:0; // IMAGE_FILE_LARGE_ADDRESS_AWARE
    NumAbsolute32Relocations = 0;
    SizeOfImage = Header->OptionalHeader.SizeOfImage;
    SizeOfHeaders = Header->OptionalHeader.SizeOfHeaders;
    SectionAlignment = Header->OptionalHeader.SectionAlignment;
    if (SectionAlignment == 0) SectionAlignment = 0x1000;

    fprintf(stderr, "\tImage base: 0x%" PRIx64 "\n", ImageBase);
    fprintf(stderr, "\tSize of image: 0x%x\n", (unsigned int)SizeOfImage);

#ifdef _DEBUG_PE
    {
        int i;

        for (i = 0; i < 16; i++)
        {
            if ((DataDirectory[i].VirtualAddress == 0) && (DataDirectory[i].Size == 0)) continue;

            switch (i)
            {
                case  0: fprintf(stderr, "%s:\n", "Export Table (-> .edata Section)"); break;
                case  1: fprintf(stderr, "%s:\n", "Import Table (-> .idata Section)"); break;
                case  2: fprintf(stderr, "%s:\n", "Resource Table (-> .rsrc Section)"); break;
                case  3: fprintf(stderr, "%s:\n", "Exception Table (-> .pdata Section)"); break;
                case  4: fprintf(stderr, "%s:\n", "Certificate Table"); break;
                case  5: fprintf(stderr, "%s:\n", "Base Relocation Table (-> .reloc Section)"); break;
                case  6: fprintf(stderr, "%s:\n", "Debug (-> .debug Section)"); break;
                case  7: fprintf(stderr, "%s:\n", "Architecture (must be 0)"); break;
                case  8: fprintf(stderr, "%s:\n", "Global Ptr (size must be 0)"); break;
                case  9: fprintf(stderr, "%s:\n", "TLS Table (-> .tls Section)"); break;
                case 10: fprintf(stderr, "%s:\n", "Load Config Table"); break;
                case 11: fprintf(stderr, "%s:\n", "Bound Import"); break;
                case 12: fprintf(stderr, "%s:\n", "IAT"); break;
                case 13: fprintf(stderr, "%s:\n", "Delay Import Descriptor"); break;
                case 14: fprintf(stderr, "%s:\n", "CLR Runtime Header (-> .cormeta Section)"); break;
                case 15: fprintf(stderr, "%s:\n", "Reserved (must be 0)"); break;
            }
            fprintf(stderr, "Directory[%i].VirtualAddress=0x%x\n", i, DataDirectory[i].VirtualAddress);
            fprintf(stderr, "Directory[%i].Size=%i\n", i, DataDirectory[i].Size);
        }
        fprintf(stderr, "\n");
    }
#endif

    // check relocations
    relocations_stripped = 0;
    if (Header->FileHeader.Characteristics & 1) // IMAGE_FILE_RELOCS_STRIPPED
    {
        relocations_stripped = 1;
        fprintf(stderr, "Relocations were stripped from file - need external relocations\n");
    }

    num_sections = Header->FileHeader.NumberOfSections;
    SectionHeader = (P_IMAGE_SECTION_HEADER) &(PEfile[4 + sizeof(_IMAGE_FILE_HEADER) + Header->FileHeader.SizeOfOptionalHeader]);

    if (num_sections > MAX_SECTIONS / 2 ||
        num_sections == 0)
    {
        unload_file(&mf);
        fprintf(stderr, "Error: Too many or too few object table entries\n");
        return -1;
    }

    // allocate memory for the code
    {
        unsigned int Entry;
        uint_fast32_t size, section_size;
        uintptr_t address;

        SR_CodeSize = 0;
        for (Entry = 0; Entry < num_sections; Entry ++)
        {
            section_size = (SectionHeader[Entry].VirtualSize)?SectionHeader[Entry].VirtualSize:SectionHeader[Entry].SizeOfRawData;

            size = section_size / SectionAlignment;
            size *= SectionAlignment;
            if (size != section_size) size += SectionAlignment;

            section[Entry].size = (uint_fast32_t)SR_CodeSize;
            SR_CodeSize += size;
        }

        if (SR_CodeSize > UINT64_C(0x40000000))
        {
            unload_file(&mf);
            fprintf(stderr, "Error: Image too big\n");
            return -1;
        }

        SR_CodeBase = (uint8_t *) malloc((size_t)(SR_CodeSize + SectionAlignment - 1));
        if (SR_CodeBase == NULL)
        {
            unload_file(&mf);
            fprintf(stderr, "Error: Not enough memory\n");
            return -1;

        }

        address = (uintptr_t)SR_CodeBase / SectionAlignment;
        address *= SectionAlignment;
        SR_CodeRun = (address == (uintptr_t)SR_CodeBase)?((uint8_t *) address):((uint8_t *) address + SectionAlignment);

        memset(SR_CodeRun, 0, (size_t)SR_CodeSize);
    }

    // read pages into allocated memory
    {
        unsigned int Entry;

        import_list = NULL;
        FirstSectionRVA = SizeOfImage;

        for (Entry = 0; Entry < num_sections; Entry ++)
        {
            uint_fast32_t alloc_ofs;

            alloc_ofs = section[Entry].size;
            memset(&(section[Entry]), 0, sizeof(section_data));

            section[Entry].adr          = &(SR_CodeRun[alloc_ofs]);
            section[Entry].size         = (SectionHeader[Entry].VirtualSize)?SectionHeader[Entry].VirtualSize:SectionHeader[Entry].SizeOfRawData;
            section[Entry].size_in_file = SectionHeader[Entry].SizeOfRawData;
            section[Entry].virtaddr     = SectionHeader[Entry].VirtualAddress;
            section[Entry].start        = ImageBase + SectionHeader[Entry].VirtualAddress;
            section[Entry].flags        = SectionHeader[Entry].Characteristics;
            section[Entry].align        = lowest_bit_alignment(SectionHeader[Entry].VirtualAddress, (SectionAlignment > 4096)?4096:SectionAlignment);

            if (section[Entry].size_in_file > section[Entry].size)
            {
                section[Entry].size_in_file = section[Entry].size;
            }
            if ((int64_t)SectionHeader[Entry].PointerToRawData + section[Entry].size_in_file > SR_FileSize)
            {
                section[Entry].size_in_file = (SectionHeader[Entry].PointerToRawData < SR_FileSize)?(uint_fast32_t)(SR_FileSize - SectionHeader[Entry].PointerToRawData):0;
            }

            if (section[Entry].virtaddr < FirstSectionRVA)
            {
                FirstSectionRVA = section[Entry].virtaddr;
            }

            section[Entry].fixup_list   = NULL;
            section[Entry].output_list  = NULL;
            section[Entry].entry_list   = NULL;
            section[Entry].code_list    = NULL;
            section[Entry].label_list   = NULL;
            section[Entry].noret_list   = NULL;
            section[Entry].extrn_list   = NULL;
            section[Entry].alias_list   = NULL;
            section[Entry].ignored_list = NULL;
            section[Entry].replace_list = NULL;
            section[Entry].nocode_list  = NULL;
            section[Entry].export_list  = NULL;
            section[Entry].ibreg_list   = NULL;
            section[Entry].hint_list    = NULL;
            section[Entry].rvadisp_list = NULL;

            section[Entry].name[8] = 0;
            memcpy(section[Entry].name, SectionHeader[Entry].Name, 8);

            // long section name (offset into COFF string table)
            if (section[Entry].name[0] == '/' && Header->FileHeader.PointerToSymbolTable != 0)
            {
                uint64_t StringTable, NameOffset;

                StringTable = (uint64_t)Header->FileHeader.PointerToSymbolTable + 18 * (uint64_t)Header->FileHeader.NumberOfSymbols;
                NameOffset = strtoul(&(section[Entry].name[1]), NULL, 10);

                if (StringTable + NameOffset < (uint64_t)SR_FileSize)
                {
                    size_t len;

                    len = strnlen((const char *) &(MZfile[StringTable + NameOffset]), (size_t)(SR_FileSize - (StringTable + NameOffset)));
                    if (len > 0 && len < sizeof(section[Entry].name))
                    {
                        memcpy(section[Entry].name, &(MZfile[StringTable + NameOffset]), len);
                        section[Entry].name[len] = 0;
                    }
                }
            }

            if ((SectionHeader[Entry].Characteristics & 0x02000000) &&
                !((DataDirectory[DIR_BASERELOC].VirtualAddress == SectionHeader[Entry].VirtualAddress) && (DataDirectory[DIR_BASERELOC].Size != 0))
               )
            {
                // discardable section (debug information)
                section[Entry].type = ST_DEBUG;
            }
            else if (SectionHeader[Entry].Characteristics & 0x20)
            {
                // code segment
                section[Entry].type = ST_CODE;
            }
            else if ((SectionHeader[Entry].Characteristics & 0x80) && !(SectionHeader[Entry].Characteristics & 0x40))
            {
                // uninitialized data segment
                section[Entry].type = ST_UDATA;
            }
            else
            {
                // data segment
                if (SectionHeader[Entry].Characteristics & 0x80000000)
                {
                    section[Entry].type = ST_DATA;
                }
                else
                {
                    section[Entry].type = ST_RDATA;
                }

                if ((DataDirectory[DIR_BASERELOC].VirtualAddress != 0) && (DataDirectory[DIR_BASERELOC].Size != 0))
                {
                    if ((DataDirectory[DIR_BASERELOC].VirtualAddress == SectionHeader[Entry].VirtualAddress) &&
                        (DataDirectory[DIR_BASERELOC].Size <= section[Entry].size)
                       )
                    {
                        section[Entry].type = ST_RELOC;
                    }
                }

                if ((DataDirectory[DIR_RESOURCE].VirtualAddress != 0) && (DataDirectory[DIR_RESOURCE].Size != 0))
                {
                    if (DataDirectory[DIR_RESOURCE].VirtualAddress == SectionHeader[Entry].VirtualAddress)
                    {
                        section[Entry].type = ST_RSRC;
                    }
                }
            }

#ifdef _DEBUG_PE
            {
                fprintf(stderr, "name: %s (seg%.2i)\n", section[Entry].name, Entry + 1);
                fprintf(stderr, "VirtualSize: %i\n", SectionHeader[Entry].VirtualSize);
                fprintf(stderr, "SizeOfRawData: %i\n", SectionHeader[Entry].SizeOfRawData);
                fprintf(stderr, "VirtualAddress: 0x%x\n", SectionHeader[Entry].VirtualAddress);
                fprintf(stderr, "PointerToRawData: 0x%x\n", SectionHeader[Entry].PointerToRawData);
                fprintf(stderr, "Characteristics: 0x%x\n", SectionHeader[Entry].Characteristics);
            }
#endif

            if (section[Entry].size_in_file != 0 &&
                SectionHeader[Entry].PointerToRawData != 0)
            {
                memcpy(section[Entry].adr, &(MZfile[SectionHeader[Entry].PointerToRawData]), section[Entry].size_in_file);
            }

            orig_sections[Entry].virtaddr = section[Entry].virtaddr;
            orig_sections[Entry].size = section[Entry].size;
            orig_sections[Entry].adr = section[Entry].adr;
        }

        num_orig_sections = num_sections;
    }

    unload_file(&mf);

    // try to split uninitialized data into bss section
    if (LoadBssBorder())
    {
        fprintf(stderr, "Error: error loading bss border\n");
        return -1;
    }

    // move export table, import table and import address table to separate sections
    if (carve_section_range(DataDirectory[DIR_EXPORT].VirtualAddress, DataDirectory[DIR_EXPORT].Size, ST_EDATA, "export table") ||
        carve_section_range(DataDirectory[DIR_IMPORT].VirtualAddress, DataDirectory[DIR_IMPORT].Size, ST_IDATA, "import table") ||
        carve_section_range(DataDirectory[DIR_IAT].VirtualAddress, DataDirectory[DIR_IAT].Size, ST_IDATA, "import address table")
       )
    {
        return -1;
    }


    if (DataDirectory[DIR_TLS].VirtualAddress != 0 && DataDirectory[DIR_TLS].Size != 0)
    {
        fprintf(stderr, "Warning: executable uses static TLS - TLS directory at 0x%" PRIx64 " must be provided to the linker as _tls_used (global_aliases.sci)\n", ImageBase + DataDirectory[DIR_TLS].VirtualAddress);
    }

    // get start address
    if (!SR_get_section_reladr_exact(ImageBase + EntryPointRVA, &EIPObjectNum, &EIP))
    {
        EIPObjectNum = 0;
        EIP = 0;
    }
    else
    {
        fprintf(stderr, "\tEntry point: 0x%" PRIx64 "\n", ImageBase + EntryPointRVA);
    }

    // create fixup table
    if (relocations_stripped)
    {
        int ret;

        ret = LoadRelocations();
        if (ret == -1)
        {
            if (generate_relocations_name != NULL)
            {
                ret = GenerateRelocations(generate_relocations_name);
            }
            else
            {
                fprintf(stderr, "Error: error opening external relocations file: %s\n", "relocations.csv");
                fprintf(stderr, "Use option --generate_relocations=<file> to guess relocations\n");
            }
        }

        if (ret)
        {
            fprintf(stderr, "Error: error loading external relocations\n");
            return -1;
        }
    }
    else if ((DataDirectory[DIR_BASERELOC].VirtualAddress != 0) && (DataDirectory[DIR_BASERELOC].Size != 0))
    {
        if (LoadBaseRelocations(DataDirectory[DIR_BASERELOC].VirtualAddress, DataDirectory[DIR_BASERELOC].Size))
        {
            return -1;
        }
    }

    // read imports
    if ((DataDirectory[DIR_IMPORT].VirtualAddress != 0) && (DataDirectory[DIR_IMPORT].Size != 0))
    {
        if (LoadImports(DataDirectory[DIR_IMPORT].VirtualAddress))
        {
            return -1;
        }
    }

    // read exports
    if ((DataDirectory[DIR_EXPORT].VirtualAddress != 0) && (DataDirectory[DIR_EXPORT].Size != 0))
    {
        if (LoadExports(DataDirectory[DIR_EXPORT].VirtualAddress, DataDirectory[DIR_EXPORT].Size))
        {
            return -1;
        }
    }

    // exception directory
    ExceptionDirRVA = DataDirectory[DIR_EXCEPTION].VirtualAddress;
    ExceptionDirSize = DataDirectory[DIR_EXCEPTION].Size;

    // delay load import directory
    DelayImportDirRVA = DataDirectory[DIR_DELAYIMPORT].VirtualAddress;
    DelayImportDirSize = DataDirectory[DIR_DELAYIMPORT].Size;

    if (!LargeAddressAware)
    {
        fprintf(stderr, "Note: executable is not large address aware (/LARGEADDRESSAWARE:NO) - it can store pointers in 32 bits\n");
        fprintf(stderr, "\tWindows: link the recompiled executable with /LARGEADDRESSAWARE:NO (link.exe, lld-link) or clear the flag afterwards (editbin)\n");
        fprintf(stderr, "\tLinux: run the recompiled executable using SR64-loader (keeps all memory below 2 GB)\n");
    }
    if (NumAbsolute32Relocations)
    {
        fprintf(stderr, "Note: executable contains %u 32-bit absolute addresses - the recompiled executable must be linked below 2 GB\n", NumAbsolute32Relocations);
        fprintf(stderr, "\tWindows: link with /BASE:0x400000 (link.exe, lld-link) or --image-base=0x400000 (GNU ld); Linux: link with -no-pie\n");
    }

    return 0;
}
