"""Decrypt and decompress a retail XEX2 (basic compression) into a flat PE image.

Usage: xexdecrypt.py <in.xex> <out.pe>

The retail key is not shipped with this project; it is read from the XenonRecomp checkout
(thirdparty/XenonRecomp/XenonUtils/xex.h), which must be cloned first.

AES comes from pycryptodome when it is installed, else from the system's OpenSSL library (libcrypto, through
ctypes; HC09_LIBCRYPTO=<path> names it), else from the openssl command: SteamOS has the last two but not the first.
"""
import ctypes
import ctypes.util
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys

XEX_H = pathlib.Path(__file__).resolve().parent.parent / "thirdparty/XenonRecomp/XenonUtils/xex.h"
DEVKIT_KEY = bytes(16)


def load_retail_key():
    try:
        text = XEX_H.read_text()
    except OSError:
        sys.exit(f"{XEX_H} not found: clone XenonRecomp into thirdparty/XenonRecomp first")
    m = re.search(r"Xex2RetailKey\[16\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        sys.exit(f"Xex2RetailKey not found in {XEX_H}")
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def _aes_pycryptodome(key, iv, data):
    from Crypto.Cipher import AES
    cipher = AES.new(key, AES.MODE_ECB) if iv is None else AES.new(key, AES.MODE_CBC, iv=iv)
    return cipher.decrypt(data)


def _aes_libcrypto(key, iv, data):
    name = os.environ.get("HC09_LIBCRYPTO") or ctypes.util.find_library("crypto")
    if not name:
        raise OSError("libcrypto not found")
    lib = ctypes.CDLL(name)
    lib.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
    lib.EVP_aes_128_ecb.restype = ctypes.c_void_p
    lib.EVP_aes_128_cbc.restype = ctypes.c_void_p
    lib.EVP_DecryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.EVP_CIPHER_CTX_set_padding.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.EVP_DecryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int]
    lib.EVP_DecryptFinal_ex.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    lib.EVP_CIPHER_CTX_free.argtypes = [ctypes.c_void_p]
    ctx = lib.EVP_CIPHER_CTX_new()
    try:
        cipher = lib.EVP_aes_128_ecb() if iv is None else lib.EVP_aes_128_cbc()
        if lib.EVP_DecryptInit_ex(ctx, cipher, None, key, iv) != 1:
            raise OSError("EVP_DecryptInit_ex failed")
        lib.EVP_CIPHER_CTX_set_padding(ctx, 0)
        out = ctypes.create_string_buffer(len(data) + 16)
        size, tail = ctypes.c_int(0), ctypes.c_int(0)
        if lib.EVP_DecryptUpdate(ctx, out, ctypes.byref(size), data, len(data)) != 1:
            raise OSError("EVP_DecryptUpdate failed")
        last = ctypes.create_string_buffer(32)
        if lib.EVP_DecryptFinal_ex(ctx, last, ctypes.byref(tail)) != 1:
            raise OSError("EVP_DecryptFinal_ex failed")
        return out.raw[: size.value] + last.raw[: tail.value]
    finally:
        lib.EVP_CIPHER_CTX_free(ctx)


def _aes_openssl(key, iv, data):
    tool = shutil.which("openssl")
    if not tool:
        raise OSError("openssl not found")
    args = [tool, "enc", "-d", "-nopad", "-K", key.hex()]
    args += ["-aes-128-ecb"] if iv is None else ["-aes-128-cbc", "-iv", iv.hex()]
    return subprocess.run(args, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout


def aes_decrypt(key, iv, data):
    """AES-128 decryption without padding: ECB when iv is None, else CBC."""
    errors = []
    for backend in (_aes_pycryptodome, _aes_libcrypto, _aes_openssl):
        try:
            plain = backend(key, iv, data)
            if len(plain) == len(data):
                return plain
            errors.append(f"{backend.__name__}: {len(plain)} bytes out of {len(data)}")
        except (ImportError, OSError, subprocess.CalledProcessError) as error:
            errors.append(f"{backend.__name__}: {error}")
    sys.exit("no AES implementation available (install pycryptodome or OpenSSL):\n  " + "\n  ".join(errors))


def find_header(d, key):
    count = struct.unpack_from(">I", d, 0x14)[0]
    for i in range(count):
        k, v = struct.unpack_from(">II", d, 0x18 + i * 8)
        if k == key:
            return v
    return None


def decrypt(d):
    retail_key = load_retail_key()
    pe_off, sec_off = struct.unpack_from(">I", d, 8)[0], struct.unpack_from(">I", d, 0x10)[0]
    ffi = find_header(d, 0x000003FF)
    info_size, enc, comp = struct.unpack_from(">IHH", d, ffi)
    payload = d[pe_off:]
    if enc:
        wrapped = d[sec_off + 0x150: sec_off + 0x160]
        for master in (retail_key, DEVKIT_KEY):
            session = aes_decrypt(master, None, wrapped)
            usable = len(payload) // 16 * 16
            plain = aes_decrypt(session, bytes(16), payload[:usable]) + payload[usable:]
            if comp != 1 or plain[:2] == b"MZ":
                break
        payload = plain
    if comp == 0:
        return payload
    if comp != 1:
        raise NotImplementedError("only basic (raw block) compression is supported")
    out, src = bytearray(), 0
    for i in range((info_size - 8) // 8):
        data_size, zero_size = struct.unpack_from(">II", d, ffi + 8 + i * 8)
        out += payload[src: src + data_size]
        out += bytes(zero_size)
        src += data_size
    return bytes(out)


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    image = decrypt(data)
    if image[:2] != b"MZ":
        sys.exit("decryption failed: no MZ header")
    open(sys.argv[2], "wb").write(image)
    print(f"wrote {len(image):#x} bytes")
