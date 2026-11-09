import ctypes, sys, os
from ctypes import wintypes

# Embed RT_MANIFEST (type 24, id 1) into a PE exe/dll using UpdateResource.
# Usage: python embed_manifest.py <exe> <manifest.xml>

def main():
    if len(sys.argv) != 3:
        print("usage: embed_manifest.py <exe> <manifest.xml>")
        return 2
    exe = sys.argv[1]
    manifest = sys.argv[2]
    with open(manifest, 'rb') as f:
        data = f.read()
    # ensure no UTF-8 BOM issues? fine as-is.
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    BeginUpdateResourceW = k32.BeginUpdateResourceW
    BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    BeginUpdateResourceW.restype = wintypes.HANDLE
    UpdateResourceW = k32.UpdateResourceW
    UpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.WORD, wintypes.LPVOID, wintypes.DWORD]
    UpdateResourceW.restype = wintypes.BOOL
    EndUpdateResourceW = k32.EndUpdateResourceW
    EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    EndUpdateResourceW.restype = wintypes.BOOL

    RT_MANIFEST = wintypes.LPCWSTR(24)  # MAKEINTRESOURCE(24)
    ID = wintypes.LPCWSTR(1)            # CREATEPROCESS_MANIFEST_RESOURCE_ID
    LANG_NEUTRAL = 0

    h = BeginUpdateResourceW(exe, False)
    if not h:
        raise ctypes.WinError(ctypes.get_last_error())
    buf = ctypes.create_string_buffer(data)
    if not UpdateResourceW(h, RT_MANIFEST, ID, LANG_NEUTRAL, buf, len(data)):
        err = ctypes.get_last_error()
        EndUpdateResourceW(h, True)
        raise ctypes.WinError(err)
    if not EndUpdateResourceW(h, False):
        raise ctypes.WinError(ctypes.get_last_error())
    print(f"embedded manifest ({len(data)} bytes) into {exe}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
