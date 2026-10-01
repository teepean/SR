# Interstate '76 runtime: imports used by the recompiled modules.
# name  conv  flags  dll   # modules | IDA prototype
# conv: stdN = stdcall with N dword args, cN = cdecl, vN = cdecl varargs with N fixed args,
#       f1f/f2f = FPU intrinsic (st0 / st1,st0 -> st0), data = data symbol, raw = hand-written asm,
#       module = provided by another recompiled module (no stub)
# flags: fret = returns double in st0, u64 = returns a 64-bit value in edx:eax
??2@YAPAXI@Z                 c1     -     MSVCRT.dll     # exe,shell |
??3@YAXPAX@Z                 c1     -     MSVCRT.dll     # exe,shell |
__CxxFrameHandler            raw    -     MSVCRT.dll     # exe,shell |
__mb_cur_max                 data   -     MSVCRT.dll     # exe | int
_access                      c2     -     MSVCRT.dll     # exe | int (__cdecl *)(const char *FileName, int AccessMode)
_assert                      c3     -     MSVCRT.dll     # shell |
_CIacos                      f1f    -     MSVCRT.dll     # exe |
_CIasin                      f1f    -     MSVCRT.dll     # exe |
_CIatan2                     f2f    -     MSVCRT.dll     # exe |
_CIfmod                      f2f    -     MSVCRT.dll     # exe |
_CIpow                       f2f    -     MSVCRT.dll     # exe |
_close                       c1     -     MSVCRT.dll     # exe | int (__cdecl *)(int FileHandle)
_errno                       c0     -     MSVCRT.dll     # exe | int *(__cdecl *)()
_except_handler3             raw    -     MSVCRT.dll     # exe |
_findclose                   c1     -     MSVCRT.dll     # shell | int (__cdecl *)(intptr_t FindHandle)
_findfirst                   c2     -     MSVCRT.dll     # shell |
_findnext                    c2     -     MSVCRT.dll     # shell |
_ftol                        raw    -     MSVCRT.dll     # exe,shell |
_grAlphaBlendFunction@16     std4   -     glide2x.dll    # zglide |
_grAlphaCombine@20           std5   -     glide2x.dll    # zglide |
_grBufferClear@12            std3   -     glide2x.dll    # zglide |
_grBufferSwap@4              std1   -     glide2x.dll    # zglide |
_grChromakeyMode@4           std1   -     glide2x.dll    # zglide |
_grChromakeyValue@4          std1   -     glide2x.dll    # zglide |
_grColorCombine@20           std5   -     glide2x.dll    # zglide |
_grConstantColorValue@4      std1   -     glide2x.dll    # zglide |
_grCullMode@4                std1   -     glide2x.dll    # zglide |
_grDepthBufferFunction@4     std1   -     glide2x.dll    # zglide |
_grDepthBufferMode@4         std1   -     glide2x.dll    # zglide |
_grDepthMask@4               std1   -     glide2x.dll    # zglide |
_grDrawLine@8                std2   -     glide2x.dll    # zglide |
_grDrawPoint@4               std1   -     glide2x.dll    # zglide |
_grDrawTriangle@12           std3   -     glide2x.dll    # zglide |
_grFogColorValue@4           std1   -     glide2x.dll    # zglide |
_grFogMode@4                 std1   -     glide2x.dll    # zglide |
_grGlideGetState@4           std1   -     glide2x.dll    # zglide |
_grGlideInit@0               std0   -     glide2x.dll    # zglide |
_grGlideSetState@4           std1   -     glide2x.dll    # zglide |
_grGlideShutdown@0           std0   -     glide2x.dll    # zglide |
_grLfbLock@24                std6   -     glide2x.dll    # zglide |
_grLfbUnlock@8               std2   -     glide2x.dll    # zglide |
_grRenderBuffer@4            std1   -     glide2x.dll    # zglide |
_grSstQueryBoards@4          std1   -     glide2x.dll    # zglide |
_grSstQueryHardware@4        std1   -     glide2x.dll    # zglide |
_grSstSelect@4               std1   -     glide2x.dll    # zglide |
_grSstWinClose@0             std0   -     glide2x.dll    # zglide |
_grSstWinOpen@28             std7   -     glide2x.dll    # zglide |
_grTexCalcMemRequired@16     std4   -     glide2x.dll    # zglide |
_grTexClampMode@12           std3   -     glide2x.dll    # zglide |
_grTexCombine@28             std7   -     glide2x.dll    # zglide |
_grTexDownloadMipMap@16      std4   -     glide2x.dll    # zglide |
_grTexDownloadTable@12       std3   -     glide2x.dll    # zglide |
_grTexFilterMode@12          std3   -     glide2x.dll    # zglide |
_grTexMaxAddress@4           std1   -     glide2x.dll    # zglide |
_grTexMinAddress@4           std1   -     glide2x.dll    # zglide |
_grTexMipMapMode@12          std3   -     glide2x.dll    # zglide |
_grTexSource@16              std4   -     glide2x.dll    # zglide |
_iob                         data   -     MSVCRT.dll     # shell | FILE[]
_isctype                     c2     -     MSVCRT.dll     # exe | int (__cdecl *)(int C, int Type)
_ismbcalnum                  c1     -     MSVCRT.dll     # exe | int (__cdecl *)(unsigned int C)
_ismbcpunct                  c1     -     MSVCRT.dll     # exe | int (__cdecl *)(unsigned int C)
_ismbcspace                  c1     -     MSVCRT.dll     # exe | int (__cdecl *)(unsigned int C)
_isnan                       c2     -     MSVCRT.dll     # exe | int (__cdecl *)(double X)
_itoa                        c3     -     MSVCRT.dll     # shell | char *(__cdecl *)(int Value, char *Buffer, int Radix)
_lseek                       c3     -     MSVCRT.dll     # exe | int (__cdecl *)(int FileHandle, int Offset, int Origin)
_mbctype                     data   -     MSVCRT.dll     # exe | unsigned __int8[]
_mbsicmp                     c2     -     MSVCRT.dll     # exe | int (__cdecl *)(const unsigned __int8 *Str1, const unsigned __int8 *Str2)
_mbsnbcat                    c3     -     MSVCRT.dll     # exe | unsigned __int8 *(__cdecl *)(unsigned __int8 *Dest, const unsigned __int8 *Source, size_t Count)
_mbsnbcpy                    c3     -     MSVCRT.dll     # exe | unsigned __int8 *(__cdecl *)(unsigned __int8 *Dest, const unsigned __int8 *Source, size_t Count)
_mbsnbicmp                   c3     -     MSVCRT.dll     # exe | int (__cdecl *)(const unsigned __int8 *Str1, const unsigned __int8 *Str2, size_t MaxCount)
_mkdir                       c1     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(const char *Path)
_msize                       c1     -     MSVCRT.dll     # exe | size_t (__cdecl *)(void *Block)
_open                        v2     -     MSVCRT.dll     # exe | int (*)(const char *FileName, int OpenFlag, ...)
_pctype                      data   -     MSVCRT.dll     # exe | const unsigned __int16 *
_putch                       c1     -     MSVCRT.dll     # exe | int (__cdecl *)(int Ch)
_read                        c3     -     MSVCRT.dll     # exe | int (__cdecl *)(int FileHandle, void *DstBuf, unsigned int MaxCharCount)
_splitpath                   c5     -     MSVCRT.dll     # exe,shell | void (__cdecl *)(const char *FullPath, char *Drive, char *Dir, char *Filename, char *Ext)
_stat                        c2     -     MSVCRT.dll     # exe | int (__cdecl *)(const char *const FileName, struct _stat32 *const Stat)
_stricmp                     c2     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(const char *String1, const char *String2)
_strlwr                      c1     -     MSVCRT.dll     # exe,shell | char *(__cdecl *)(char *String)
_strnicmp                    c3     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(const char *String1, const char *String2, size_t MaxCount)
_unlink                      c1     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(const char *FileName)
_vsnprintf                   c4     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(char *const Buffer, const size_t BufferCount, const char *const Format, va_list ArgList)
abort                        c0     -     MSVCRT.dll     # exe | void (__cdecl __noreturn *)()
AddFontResourceA             std1   -     GDI32.dll      # exe | int (__stdcall *)(LPCSTR)
AdjustWindowRect             std3   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(LPRECT lpRect, DWORD dwStyle, BOOL bMenu)
atoi                         c1     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(const char *String)
atol                         c1     -     MSVCRT.dll     # exe | int (__cdecl *)(const char *String)
auxGetDevCapsA               std3   -     WIN32.dll      # exe | MMRESULT (__stdcall *)(UINT_PTR uDeviceID, LPAUXCAPSA pac, UINT cbac)
auxGetNumDevs                std0   -     WIN32.dll      # exe | UINT (__stdcall *)()
auxSetVolume                 std2   -     WIN32.dll      # exe | MMRESULT (__stdcall *)(UINT uDeviceID, DWORD dwVolume)
BeginPaint                   std2   -     USER32.dll     # exe,shell | HDC (__stdcall *)(HWND hWnd, LPPAINTSTRUCT lpPaint)
BitBlt                       std9   -     GDI32.dll      # shell | BOOL (__stdcall *)(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
bsearch                      c5     -     MSVCRT.dll     # exe,shell | void *(__cdecl *)(const void *Key, const void *Base, size_t NumOfElements, size_t SizeOfElements, _CoreCrtNonSecureSearchSortCompareFunction CompareFunction)
calloc                       c2     -     MSVCRT.dll     # shell | void *(__cdecl *)(size_t Count, size_t Size)
ClientToScreen               std2   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(HWND hWnd, LPPOINT lpPoint)
ClipCursor                   std1   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(const RECT *lpRect)
clock                        c0     -     MSVCRT.dll     # shell | clock_t (__cdecl *)()
CloseHandle                  std1   -     KERNEL32.dll   # exe,shell,zglide | BOOL (__stdcall *)(HANDLE hObject)
CoInitialize                 std1   -     ole32.dll      # exe | HRESULT (__stdcall *)(LPVOID pvReserved)
CopyFileA                    std3   -     KERNEL32.dll   # exe,shell | BOOL (__stdcall *)(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists)
CoUninitialize               std0   -     ole32.dll      # exe | void (__stdcall *)()
CreateCompatibleDC           std1   -     GDI32.dll      # exe,shell | HDC (__stdcall *)(HDC hdc)
CreateDIBSection             std6   -     GDI32.dll      # exe,shell | HBITMAP (__stdcall *)(HDC hdc, const BITMAPINFO *pbmi, UINT usage, void **ppvBits, HANDLE hSection, DWORD offset)
CreateFileA                  std7   -     KERNEL32.dll   # exe,shell,zglide | HANDLE (__stdcall *)(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
CreateFileMappingA           std6   -     KERNEL32.dll   # exe | HANDLE (__stdcall *)(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName)
CreateFontIndirectA          std1   -     GDI32.dll      # exe | HFONT (__stdcall *)(const LOGFONTA *lplf)
CreatePalette                std1   -     GDI32.dll      # exe,shell | HPALETTE (__stdcall *)(const LOGPALETTE *plpal)
CreateProcessA               std10  -     KERNEL32.dll   # shell | BOOL (__stdcall *)(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
CreateScalableFontResourceA  std4   -     GDI32.dll      # exe | BOOL (__stdcall *)(DWORD fdwHidden, LPCSTR lpszFont, LPCSTR lpszFile, LPCSTR lpszPath)
CreateThread                 std6   -     KERNEL32.dll   # shell | HANDLE (__stdcall *)(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
CreateWindowExA              std12  -     USER32.dll     # exe,shell | HWND (__stdcall *)(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
DefWindowProcA               std4   -     USER32.dll     # exe,shell | LRESULT (__stdcall *)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
DeleteCriticalSection        std1   -     KERNEL32.dll   # zglide | void (__stdcall *)(LPCRITICAL_SECTION lpCriticalSection)
DeleteDC                     std1   -     GDI32.dll      # exe,shell | BOOL (__stdcall *)(HDC hdc)
DeleteFileA                  std1   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(LPCSTR lpFileName)
DeleteObject                 std1   -     GDI32.dll      # exe,shell | BOOL (__stdcall *)(HGDIOBJ ho)
DestroyWindow                std1   -     USER32.dll     # exe | BOOL (__stdcall *)(HWND hWnd)
DialogBoxParamA              std5   -     USER32.dll     # shell | INT_PTR (__stdcall *)(HINSTANCE hInstance, LPCSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
difftime                     c2     fret  MSVCRT.dll     # shell | double (__cdecl *)(const __time32_t Time1, const __time32_t Time2)
DirectDrawCreate             std3   -     DDRAW.dll      # exe | HRESULT (__stdcall *)(GUID *lpGUID, LPDIRECTDRAW *lplpDD, IUnknown *pUnkOuter)
DirectDrawEnumerateA         std2   -     DDRAW.dll      # exe | HRESULT (__stdcall *)(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext)
DirectSoundCreate            std3   -     DSOUND.dll     # exe | HRESULT (__stdcall *)(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter)
DispatchMessageA             std1   -     USER32.dll     # exe,shell | LRESULT (__stdcall *)(const MSG *lpMsg)
msvcrt_div                   c2     u64   MSVCRT.dll     # exe | div_t (__cdecl *)(int Numerator, int Denominator)
dp_enableDebugPrint          module -     anetdll.dll       # shell |
dpAddPlayerToGroup           module -     anetdll.dll       # exe |
dpClose                      module -     anetdll.dll       # shell |
dpCreate                     module -     anetdll.dll       # shell |
dpCreateGroup                module -     anetdll.dll       # shell |
dpCreatePlayer               module -     anetdll.dll       # shell |
dpDestroy                    module -     anetdll.dll       # shell |
dpDestroyPlayer              module -     anetdll.dll       # exe,shell |
dpEnumGroupPlayers           module -     anetdll.dll       # exe |
dpEnumGroups                 module -     anetdll.dll       # exe |
dpEnumPlayers                module -     anetdll.dll       # exe,shell |
dpEnumSessions               module -     anetdll.dll       # shell |
dpFreeze                     module -     anetdll.dll       # shell (dead code) |
dpEnumTransports             module -     anetdll.dll       # shell |
dpGetPlayerData              module -     anetdll.dll       # exe,shell |
dpGetPlayerName              module -     anetdll.dll       # exe |
dpGetSessionDesc             module -     anetdll.dll       # shell |
dpOpen                       module -     anetdll.dll       # shell |
dpReceive                    module -     anetdll.dll       # exe,shell |
dpSend                       module -     anetdll.dll       # exe,shell |
dpSetGameServer              module -     anetdll.dll       # shell |
dpSetPlayerData              module -     anetdll.dll       # exe,shell |
EndDialog                    std2   -     USER32.dll     # shell | BOOL (__stdcall *)(HWND hDlg, INT_PTR nResult)
EndPaint                     std2   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(HWND hWnd, const PAINTSTRUCT *lpPaint)
EnterCriticalSection         std1   -     KERNEL32.dll   # zglide | void (__stdcall *)(LPCRITICAL_SECTION lpCriticalSection)
exit                         c1     -     MSVCRT.dll     # exe,shell | void (__cdecl __noreturn *)(int Code)
ExitProcess                  std1   -     KERNEL32.dll   # zglide | void (__stdcall __noreturn *)(UINT uExitCode)
fclose                       c1     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(FILE *Stream)
fflush                       c1     -     MSVCRT.dll     # shell | int (__cdecl *)(FILE *Stream)
fgetc                        c1     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(FILE *Stream)
fgets                        c3     -     MSVCRT.dll     # exe,shell | char *(__cdecl *)(char *Buffer, int MaxCount, FILE *Stream)
FindClose                    std1   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(HANDLE hFindFile)
FindFirstFileA               std2   -     KERNEL32.dll   # exe | HANDLE (__stdcall *)(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
FindNextFileA                std2   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
FindWindowA                  std2   -     USER32.dll     # exe | HWND (__stdcall *)(LPCSTR lpClassName, LPCSTR lpWindowName)
floor                        c2     fret  MSVCRT.dll     # exe | double (__cdecl *)(double X)
FlushFileBuffers             std1   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(HANDLE hFile)
fopen                        c2     -     MSVCRT.dll     # exe,shell | FILE *(__cdecl *)(const char *FileName, const char *Mode)
fprintf                      v2     -     MSVCRT.dll     # exe,shell | int (*)(FILE *const Stream, const char *const Format, ...)
fputc                        c2     -     MSVCRT.dll     # exe | int (__cdecl *)(int Character, FILE *Stream)
fputs                        c2     -     MSVCRT.dll     # shell | int (__cdecl *)(const char *Buffer, FILE *Stream)
fread                        c4     -     MSVCRT.dll     # exe,shell | size_t (__cdecl *)(void *Buffer, size_t ElementSize, size_t ElementCount, FILE *Stream)
free                         c1     -     MSVCRT.dll     # shell | void (__cdecl *)(void *Block)
FreeLibrary                  std1   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(HMODULE hLibModule)
fscanf                       v2     -     MSVCRT.dll     # exe | int (*)(FILE *const Stream, const char *const Format, ...)
fseek                        c3     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(FILE *Stream, int Offset, int Origin)
ftell                        c1     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(FILE *Stream)
fwrite                       c4     -     MSVCRT.dll     # exe,shell | size_t (__cdecl *)(const void *Buffer, size_t ElementSize, size_t ElementCount, FILE *Stream)
GdiFlush                     std0   -     GDI32.dll      # exe | BOOL (__stdcall *)()
GetAsyncKeyState             std1   -     USER32.dll     # exe,shell | SHORT (__stdcall *)(int vKey)
getc                         c1     -     MSVCRT.dll     # shell | int (__cdecl *)(FILE *Stream)
GetClientRect                std2   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(HWND hWnd, LPRECT lpRect)
GetCPInfo                    std2   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(UINT CodePage, LPCPINFO lpCPInfo)
GetCurrentDirectoryA         std2   -     KERNEL32.dll   # exe | DWORD (__stdcall *)(DWORD nBufferLength, LPSTR lpBuffer)
GetCurrentProcess            std0   -     KERNEL32.dll   # exe,zglide | HANDLE (__stdcall *)()
GetCurrentThreadId           std0   -     KERNEL32.dll   # zglide | DWORD (__stdcall *)()
GetCursorPos                 std1   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(LPPOINT lpPoint)
GetDC                        std1   -     USER32.dll     # exe,shell | HDC (__stdcall *)(HWND hWnd)
GetDeviceCaps                std2   -     GDI32.dll      # exe,shell | int (__stdcall *)(HDC hdc, int index)
GetDlgItem                   std2   -     USER32.dll     # shell | HWND (__stdcall *)(HWND hDlg, int nIDDlgItem)
GetDlgItemTextA              std4   -     USER32.dll     # shell | UINT (__stdcall *)(HWND hDlg, int nIDDlgItem, LPSTR lpString, int cchMax)
GetDriveTypeA                std1   -     KERNEL32.dll   # exe,shell | UINT (__stdcall *)(LPCSTR lpRootPathName)
GetFileTime                  std4   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime)
GetFileType                  std1   -     KERNEL32.dll   # zglide | DWORD (__stdcall *)(HANDLE hFile)
GetFocus                     std0   -     USER32.dll     # exe | HWND (__stdcall *)()
GetKeyboardType              std1   -     USER32.dll     # exe | int (__stdcall *)(int nTypeFlag)
GetKeyState                  std1   -     USER32.dll     # exe,shell | SHORT (__stdcall *)(int nVirtKey)
GetLastError                 std0   -     KERNEL32.dll   # shell,zglide | DWORD (__stdcall *)()
GetLocaleInfoA               std4   -     KERNEL32.dll   # zglide | int (__stdcall *)(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData)
GetLocaleInfoW               std4   -     KERNEL32.dll   # zglide | int (__stdcall *)(LCID Locale, LCTYPE LCType, LPWSTR lpLCData, int cchData)
GetLogicalDrives             std0   -     KERNEL32.dll   # exe | DWORD (__stdcall *)()
GetLogicalDriveStringsA      std2   -     KERNEL32.dll   # shell | DWORD (__stdcall *)(DWORD nBufferLength, LPSTR lpBuffer)
GetModuleFileNameA           std3   -     KERNEL32.dll   # zglide | DWORD (__stdcall *)(HMODULE hModule, LPSTR lpFilename, DWORD nSize)
GetModuleHandleA             std1   -     KERNEL32.dll   # exe,zglide | HMODULE (__stdcall *)(LPCSTR lpModuleName)
GetProcAddress               std2   -     KERNEL32.dll   # exe,zglide | FARPROC (__stdcall *)(HMODULE hModule, LPCSTR lpProcName)
GetProcessHeap               std0   -     KERNEL32.dll   # exe | HANDLE (__stdcall *)()
GetStdHandle                 std1   -     KERNEL32.dll   # zglide | HANDLE (__stdcall *)(DWORD nStdHandle)
GetStockObject               std1   -     GDI32.dll      # exe | HGDIOBJ (__stdcall *)(int i)
GetStringTypeA               std5   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(LCID Locale, DWORD dwInfoType, LPCSTR lpSrcStr, int cchSrc, LPWORD lpCharType)
GetStringTypeW               std4   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(DWORD dwInfoType, LPCWCH lpSrcStr, int cchSrc, LPWORD lpCharType)
GetSystemDefaultLCID         std0   -     KERNEL32.dll   # exe | LCID (__stdcall *)()
GetSystemInfo                std1   -     KERNEL32.dll   # exe | void (__stdcall *)(LPSYSTEM_INFO lpSystemInfo)
GetSystemMetrics             std1   -     USER32.dll     # exe,shell | int (__stdcall *)(int nIndex)
GetSystemPaletteEntries      std4   -     GDI32.dll      # exe,shell | UINT (__stdcall *)(HDC hdc, UINT iStart, UINT cEntries, LPPALETTEENTRY pPalEntries)
GetTextExtentExPointA        std7   -     GDI32.dll      # exe | BOOL (__stdcall *)(HDC hdc, LPCSTR lpszString, int cchString, int nMaxExtent, LPINT lpnFit, LPINT lpnDx, LPSIZE lpSize)
GetTextExtentPoint32A        std4   -     GDI32.dll      # exe | BOOL (__stdcall *)(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl)
GetTickCount                 std0   -     KERNEL32.dll   # exe,shell | DWORD (__stdcall *)()
GetVolumeInformationA        std8   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize, LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize)
GetWindowLongA               std2   -     USER32.dll     # shell | LONG (__stdcall *)(HWND hWnd, int nIndex)
GetWindowRect                std2   -     USER32.dll     # exe | BOOL (__stdcall *)(HWND hWnd, LPRECT lpRect)
GetWindowsDirectoryA         std2   -     KERNEL32.dll   # shell | UINT (__stdcall *)(LPSTR lpBuffer, UINT uSize)
HeapAlloc                    std3   -     KERNEL32.dll   # exe,shell,zglide | LPVOID (__stdcall *)(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes)
HeapCompact                  std2   -     KERNEL32.dll   # exe | SIZE_T (__stdcall *)(HANDLE hHeap, DWORD dwFlags)
HeapCreate                   std3   -     KERNEL32.dll   # exe,shell | HANDLE (__stdcall *)(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize)
HeapDestroy                  std1   -     KERNEL32.dll   # exe,shell | BOOL (__stdcall *)(HANDLE hHeap)
HeapFree                     std3   -     KERNEL32.dll   # exe,shell,zglide | BOOL (__stdcall *)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem)
HeapReAlloc                  std4   -     KERNEL32.dll   # exe,shell | LPVOID (__stdcall *)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes)
HeapSize                     std3   -     KERNEL32.dll   # exe | SIZE_T (__stdcall *)(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem)
ImmAssociateContext          std2   -     IMM32.dll      # exe | HIMC (__stdcall *)(HWND, HIMC)
InitializeCriticalSection    std1   -     KERNEL32.dll   # zglide | void (__stdcall *)(LPCRITICAL_SECTION lpCriticalSection)
InterlockedDecrement         std1   -     KERNEL32.dll   # zglide | LONG (__stdcall *)(volatile LONG *lpAddend)
InterlockedIncrement         std1   -     KERNEL32.dll   # zglide | LONG (__stdcall *)(volatile LONG *lpAddend)
isspace                      c1     -     MSVCRT.dll     # shell | int (__cdecl *)(int C)
joyGetDevCapsA               std3   -     WIN32.dll      # exe,shell | MMRESULT (__stdcall *)(UINT_PTR uJoyID, LPJOYCAPSA pjc, UINT cbjc)
joyGetNumDevs                std0   -     WIN32.dll      # exe,shell | UINT (__stdcall *)()
joyGetPos                    std2   -     WINMM.dll      # shell | MMRESULT (__stdcall *)(UINT uJoyID, LPJOYINFO pji)
joyGetPosEx                  std2   -     WIN32.dll      # exe,shell | MMRESULT (__stdcall *)(UINT uJoyID, LPJOYINFOEX pji)
LCMapStringA                 std6   -     KERNEL32.dll   # zglide | int (__stdcall *)(LCID Locale, DWORD dwMapFlags, LPCSTR lpSrcStr, int cchSrc, LPSTR lpDestStr, int cchDest)
LCMapStringW                 std6   -     KERNEL32.dll   # zglide | int (__stdcall *)(LCID Locale, DWORD dwMapFlags, LPCWSTR lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest)
LeaveCriticalSection         std1   -     KERNEL32.dll   # zglide | void (__stdcall *)(LPCRITICAL_SECTION lpCriticalSection)
LoadCursorA                  std2   -     USER32.dll     # exe,shell | HCURSOR (__stdcall *)(HINSTANCE hInstance, LPCSTR lpCursorName)
LoadCursorFromFileA          std1   -     USER32.dll     # shell | HCURSOR (__stdcall *)(LPCSTR lpFileName)
LoadLibraryA                 std1   -     KERNEL32.dll   # exe,zglide | HMODULE (__stdcall *)(LPCSTR lpLibFileName)
LoadModule                   std2   -     KERNEL32.dll   # shell | DWORD (__stdcall *)(LPCSTR lpModuleName, LPVOID lpParameterBlock)
lstrcatA                     std2   -     KERNEL32.dll   # exe | LPSTR (__stdcall *)(LPSTR lpString1, LPCSTR lpString2)
lstrcpyA                     std2   -     KERNEL32.dll   # exe | LPSTR (__stdcall *)(LPSTR lpString1, LPCSTR lpString2)
malloc                       c1     -     MSVCRT.dll     # exe,shell | void *(__cdecl *)(size_t Size)
MapViewOfFile                std5   -     KERNEL32.dll   # exe | LPVOID (__stdcall *)(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap)
MapVirtualKeyA               std2   -     USER32.dll     # exe | UINT (__stdcall *)(UINT uCode, UINT uMapType)
mciGetErrorStringA           std3   -     WIN32.dll      # exe | BOOL (__stdcall *)(MCIERROR mcierr, LPSTR pszText, UINT cchText)
mciSendCommandA              std4   -     WIN32.dll      # exe | MCIERROR (__stdcall *)(MCIDEVICEID mciId, UINT uMsg, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
memmove                      c3     -     MSVCRT.dll     # exe,shell | void *(__cdecl *)(void *, const void *Src, size_t Size)
MessageBoxA                  std4   -     USER32.dll     # exe,shell | int (__stdcall *)(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
MultiByteToWideChar          std6   -     KERNEL32.dll   # zglide | int (__stdcall *)(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar)
OutputDebugStringA           std1   -     KERNEL32.dll   # exe,shell | void (__stdcall *)(LPCSTR lpOutputString)
PeekMessageA                 std5   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
PostMessageA                 std4   -     USER32.dll     # shell | BOOL (__stdcall *)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
PostQuitMessage              std1   -     USER32.dll     # shell | void (__stdcall *)(int nExitCode)
qsort                        c4     -     MSVCRT.dll     # exe,shell | void (__cdecl *)(void *Base, size_t NumOfElements, size_t SizeOfElements, _CoreCrtNonSecureSearchSortCompareFunction CompareFunction)
rand                         c0     -     MSVCRT.dll     # exe,shell | int (__cdecl *)()
ReadFile                     std5   -     KERNEL32.dll   # shell,zglide | BOOL (__stdcall *)(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
RealizePalette               std1   -     GDI32.dll      # exe,shell | UINT (__stdcall *)(HDC hdc)
realloc                      c2     -     MSVCRT.dll     # shell | void *(__cdecl *)(void *Block, size_t Size)
Rectangle                    std5   -     GDI32.dll      # exe | BOOL (__stdcall *)(HDC hdc, int left, int top, int right, int bottom)
RegCloseKey                  std1   -     ADVAPI32.dll   # exe,shell | LSTATUS (__stdcall *)(HKEY hKey)
RegCreateKeyExA              std9   -     ADVAPI32.dll   # exe | LSTATUS (__stdcall *)(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions, REGSAM samDesired, const LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult, LPDWORD lpdwDisposition)
RegisterClassA               std1   -     USER32.dll     # exe | ATOM (__stdcall *)(const WNDCLASSA *lpWndClass)
RegOpenKeyExA                std5   -     ADVAPI32.dll   # exe,shell | LSTATUS (__stdcall *)(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
RegQueryValueExA             std6   -     ADVAPI32.dll   # exe,shell | LSTATUS (__stdcall *)(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
RegSetValueExA               std6   -     ADVAPI32.dll   # exe | LSTATUS (__stdcall *)(HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE *lpData, DWORD cbData)
ReleaseDC                    std2   -     USER32.dll     # exe,shell | int (__stdcall *)(HWND hWnd, HDC hDC)
RemoveFontResourceA          std1   -     GDI32.dll      # exe | BOOL (__stdcall *)(LPCSTR lpFileName)
ScreenToClient               std2   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(HWND hWnd, LPPOINT lpPoint)
SelectObject                 std2   -     GDI32.dll      # exe,shell | HGDIOBJ (__stdcall *)(HDC hdc, HGDIOBJ h)
SelectPalette                std3   -     GDI32.dll      # exe,shell | HPALETTE (__stdcall *)(HDC hdc, HPALETTE hPal, BOOL bForceBkgd)
SendDlgItemMessageA          std5   -     USER32.dll     # shell | LRESULT (__stdcall *)(HWND hDlg, int nIDDlgItem, UINT Msg, WPARAM wParam, LPARAM lParam)
SendMessageA                 std4   -     USER32.dll     # exe,shell | LRESULT (__stdcall *)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
SetBkColor                   std2   -     GDI32.dll      # exe | COLORREF (__stdcall *)(HDC hdc, COLORREF color)
SetBkMode                    std2   -     GDI32.dll      # exe | int (__stdcall *)(HDC hdc, int mode)
SetCursor                    std1   -     USER32.dll     # exe,shell | HCURSOR (__stdcall *)(HCURSOR hCursor)
SetCursorPos                 std2   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(int X, int Y)
SetDIBColorTable             std4   -     GDI32.dll      # shell | UINT (__stdcall *)(HDC hdc, UINT iStart, UINT cEntries, const RGBQUAD *prgbq)
SetDIBitsToDevice            std12  -     GDI32.dll      # exe | int (__stdcall *)(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc, int ySrc, UINT StartScan, UINT cLines, const void *lpvBits, const BITMAPINFO *lpbmi, UINT ColorUse)
SetDlgItemTextA              std3   -     USER32.dll     # shell | BOOL (__stdcall *)(HWND hDlg, int nIDDlgItem, LPCSTR lpString)
SetEndOfFile                 std1   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(HANDLE hFile)
SetFileAttributesA           std2   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(LPCSTR lpFileName, DWORD dwFileAttributes)
SetFilePointer               std4   -     KERNEL32.dll   # zglide | DWORD (__stdcall *)(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
SetFocus                     std1   -     USER32.dll     # exe,shell | HWND (__stdcall *)(HWND hWnd)
SetLastError                 std1   -     KERNEL32.dll   # zglide | void (__stdcall *)(DWORD dwErrCode)
setlocale                    c2     -     MSVCRT.dll     # exe | char *(__cdecl *)(int Category, const char *Locale)
SetMapperFlags               std2   -     GDI32.dll      # exe | DWORD (__stdcall *)(HDC hdc, DWORD flags)
SetMenu                      std2   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(HWND hWnd, HMENU hMenu)
SetPaletteEntries            std4   -     GDI32.dll      # exe,shell | UINT (__stdcall *)(HPALETTE hpal, UINT iStart, UINT cEntries, const PALETTEENTRY *pPalEntries)
SetPriorityClass             std2   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(HANDLE hProcess, DWORD dwPriorityClass)
SetRect                      std5   -     USER32.dll     # exe | BOOL (__stdcall *)(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom)
SetStdHandle                 std2   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(DWORD nStdHandle, HANDLE hHandle)
SetSystemPaletteUse          std2   -     GDI32.dll      # shell | UINT (__stdcall *)(HDC hdc, UINT use)
SetTextColor                 std2   -     GDI32.dll      # exe | COLORREF (__stdcall *)(HDC hdc, COLORREF color)
setvbuf                      c4     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(FILE *Stream, char *Buffer, int Mode, size_t Size)
SetWindowLongA               std3   -     USER32.dll     # exe,shell | LONG (__stdcall *)(HWND hWnd, int nIndex, LONG dwNewLong)
SetWindowPos                 std7   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
ShowCursor                   std1   -     USER32.dll     # exe,shell | int (__stdcall *)(BOOL bShow)
ShowWindow                   std2   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(HWND hWnd, int nCmdShow)
Sleep                        std1   -     KERNEL32.dll   # shell | void (__stdcall *)(DWORD dwMilliseconds)
SmackBufferBlit              std8   -     smackw32.DLL   # exe |
SmackBufferClose             std1   -     smackw32.DLL   # exe |
SmackBufferNewPalette        std3   -     smackw32.DLL   # exe |
SmackBufferOpen              std6   -     smackw32.DLL   # exe |
SmackBufferSetPalette        std1   -     smackw32.DLL   # exe |
SmackClose                   std1   -     smackw32.DLL   # exe |
SmackColorRemap              std4   -     smackw32.DLL   # exe |
SmackDoFrame                 std1   -     smackw32.DLL   # exe |
SmackNextFrame               std1   -     smackw32.DLL   # exe |
SmackOpen                    std3   -     smackw32.DLL   # exe |
SmackSoundOnOff              std2   -     smackw32.DLL   # exe |
SmackSoundUseDirectSound     std1   -     smackw32.DLL   # exe |
SmackToBuffer                std7   -     smackw32.DLL   # exe |
SmackToBufferRect            std2   -     smackw32.DLL   # exe |
SmackWait                    std1   -     smackw32.DLL   # exe |
sprintf                      v2     -     MSVCRT.dll     # exe,shell | int (*)(char *const Buffer, const char *const Format, ...)
srand                        c1     -     MSVCRT.dll     # exe,shell | void (__cdecl *)(unsigned int Seed)
sscanf                       v2     -     MSVCRT.dll     # exe,shell | int (*)(const char *const Buffer, const char *const Format, ...)
strchr                       c2     -     MSVCRT.dll     # exe | char *(__cdecl *)(const char *Str, int Val)
StretchBlt                   std11  -     GDI32.dll      # shell | BOOL (__stdcall *)(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
StrLookup_Global_Object      module -     Strlkup.dll    # exe |
StrLookupCreate              module -     Strlkup.dll    # exe |
StrLookupDestroy             module -     Strlkup.dll    # exe |
StrLookupFind                module -     Strlkup.dll    # exe |
StrLookupFormat              module -     Strlkup.dll    # exe |
strncmp                      c3     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(const char *Str1, const char *Str2, size_t MaxCount)
strncpy                      c3     -     MSVCRT.dll     # exe,shell | char *(__cdecl *)(char *Destination, const char *Source, size_t Count)
strpbrk                      c2     -     MSVCRT.dll     # exe | char *(__cdecl *)(const char *Str, const char *Control)
strrchr                      c2     -     MSVCRT.dll     # exe | char *(__cdecl *)(const char *Str, int Ch)
strstr                       c2     -     MSVCRT.dll     # exe,shell | char *(__cdecl *)(const char *Str, const char *SubStr)
strtok                       c2     -     MSVCRT.dll     # exe | char *(__cdecl *)(char *String, const char *Delimiter)
TerminateProcess             std2   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(HANDLE hProcess, UINT uExitCode)
TextOutA                     std5   -     GDI32.dll      # exe | BOOL (__stdcall *)(HDC hdc, int x, int y, LPCSTR lpString, int c)
time                         c1     -     MSVCRT.dll     # exe,shell | __time32_t (__cdecl *)(__time32_t *const Time)
timeGetTime                  std0   -     WIN32.dll      # exe,shell | DWORD (__stdcall *)()
TlsGetValue                  std1   -     KERNEL32.dll   # zglide | LPVOID (__stdcall *)(DWORD dwTlsIndex)
TlsSetValue                  std2   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(DWORD dwTlsIndex, LPVOID lpTlsValue)
ToAscii                      std5   -     USER32.dll     # shell | int (__stdcall *)(UINT uVirtKey, UINT uScanCode, const BYTE *lpKeyState, LPWORD lpChar, UINT uFlags)
tolower                      c1     -     MSVCRT.dll     # exe | int (__cdecl *)(int C)
toupper                      c1     -     MSVCRT.dll     # exe,shell | int (__cdecl *)(int C)
TranslateMessage             std1   -     USER32.dll     # exe,shell | BOOL (__stdcall *)(const MSG *lpMsg)
UnmapViewOfFile              std1   -     KERNEL32.dll   # exe | BOOL (__stdcall *)(LPCVOID lpBaseAddress)
UpdateWindow                 std1   -     USER32.dll     # exe | BOOL (__stdcall *)(HWND hWnd)
ValidateRect                 std2   -     USER32.dll     # exe | BOOL (__stdcall *)(HWND hWnd, const RECT *lpRect)
VirtualAlloc                 std4   -     KERNEL32.dll   # exe,zglide | LPVOID (__stdcall *)(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
VirtualFree                  std3   -     KERNEL32.dll   # exe,zglide | BOOL (__stdcall *)(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
VirtualQuery                 std3   -     KERNEL32.dll   # exe | SIZE_T (__stdcall *)(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength)
WaitForSingleObject          std2   -     KERNEL32.dll   # shell | DWORD (__stdcall *)(HANDLE hHandle, DWORD dwMilliseconds)
WideCharToMultiByte          std8   -     KERNEL32.dll   # zglide | int (__stdcall *)(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar)
WriteFile                    std5   -     KERNEL32.dll   # zglide | BOOL (__stdcall *)(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
wsprintfA                    v2     -     USER32.dll     # exe,shell | int (*)(LPSTR, LPCSTR, ...)
wvsprintfA                   std3   -     USER32.dll     # exe,shell | int (__stdcall *)(LPSTR, LPCSTR, va_list arglist)
# DLL\WINET.DLL (ANETDLL's TCP/IP transport) is implemented in winet.c; ANETDLL gets these with GetProcAddress
commDriverInfo               c2     -     static         # anetdll | int commDriverInfo(req *, resp *)
commGroupAdd                 c2     -     static         # anetdll | int commGroupAdd(req *, resp *)
commGroupAlloc               c2     -     static         # anetdll | int commGroupAlloc(req *, resp *)
commGroupFree                c2     -     static         # anetdll | int commGroupFree(req *, resp *)
commInit                     c2     -     static         # anetdll | int commInit(req *, resp *)
commNoOp                     c2     -     static         # anetdll | int commNoOp(req *, resp *)
commPeekPkt                  c2     -     static         # anetdll | int commPeekPkt(req *, resp *)
commPlayerInfo               c2     -     static         # anetdll | int commPlayerInfo(req *, resp *)
commPrintAddr                c2     -     static         # anetdll | int commPrintAddr(req *, resp *)
commRxPkt                    c2     -     static         # anetdll | int commRxPkt(req *, resp *)
commSayBye                   c2     -     static         # anetdll | int commSayBye(req *, resp *)
commSayHi                    c2     -     static         # anetdll | int commSayHi(req *, resp *)
commScanAddr                 c2     -     static         # anetdll | int commScanAddr(req *, resp *)
commSetParam                 c2     -     static         # anetdll | int commSetParam(req *, resp *)
commTerm                     c2     -     static         # anetdll | int commTerm(req *, resp *)
commTxFull                   c2     -     static         # anetdll | int commTxFull(req *, resp *)
commTxPkt                    c2     -     static         # anetdll | int commTxPkt(req *, resp *)
GlobalAlloc                  std2   -     KERNEL32.dll   # anetdll | HGLOBAL GlobalAlloc(UINT uFlags, SIZE_T dwBytes)
GlobalReAlloc                std3   -     KERNEL32.dll   # anetdll | HGLOBAL GlobalReAlloc(HGLOBAL hMem, SIZE_T dwBytes, UINT uFlags)
GlobalFree                   std1   -     KERNEL32.dll   # anetdll | HGLOBAL GlobalFree(HGLOBAL hMem)
# statically linked CRT functions of the DLLs (redirected with external_procedures.sci)
strspn                       c2     -     static         # strlkup | size_t strspn(const char *, const char *)
strcspn                      c2     -     static         # strlkup | size_t strcspn(const char *, const char *)
strncat                      c3     -     static         # strlkup | char *strncat(char *, const char *, size_t)
vsprintf                     c3     -     static         # strlkup | int vsprintf(char *, const char *, va_list)
memcpy                       c3     -     static         # zglide | void *memcpy(void *, const void *, size_t)
printf                       v1     -     static         # zglide | int printf(const char *, ...)
ungetc                       c2     -     static         # zglide | int ungetc(int, FILE *)
_filbuf                      c1     -     static         # zglide | int _filbuf(FILE *) - called by inline getc macro
_makepath                    c5     -     static         # anetdll | void _makepath(char *path, const char *drive, const char *dir, const char *fname, const char *ext)
_ftime                       c1     -     static         # anetdll | void _ftime(struct _timeb *)
_strcmpi                     c2     -     static         # anetdll | int _strcmpi(const char *, const char *)
# runtime hooks called from instruction_replacements.sci
i76_frame_tick               std0   -     runtime        # exe | GetTickCount replacement in the per-frame timer sub_49C920 (frame limiter)
i76shell_part_strncmp        c3     -     runtime        # shell | strncmp replacement in the part lookup of sub_10002130 (gamefixes.c)
