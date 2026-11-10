#!/usr/bin/env python3
"""Pack a JAR-v1 signed NativeActivity APK. The ROM is never an input."""

import base64
import datetime
import hashlib
import struct
import sys
import zipfile
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[1]
SO = ROOT / "build/android/libtarzan.so"
OUT = ROOT / "artifacts/tarzan-experimental.apk"
KEY = ROOT / "artifacts/debug.keystore.pem"
CERT = ROOT / "artifacts/debug.cert.pem"

ANDROID = "http://schemas.android.com/apk/res/android"
ATTR_ID = {
    "versionCode": 0x0101021B,
    "versionName": 0x0101021C,
    "minSdkVersion": 0x0101020C,
    "targetSdkVersion": 0x01010270,
    "glEsVersion": 0x01010281,
    "required": 0x0101028E,
    "label": 0x01010001,
    "hasCode": 0x0101000C,
    "debuggable": 0x0101000F,
    "name": 0x01010003,
    "configChanges": 0x0101001F,
    "exported": 0x01010010,
    "value": 0x01010024,
}


def u16(n):
    return struct.pack("<H", n)


def u32(n):
    return struct.pack("<I", n & 0xFFFFFFFF)


def chunk(typ, header_size, payload):
    # payload is the bytes after the 8-byte ResChunk_header.
    extra = header_size - 8
    if len(payload) < extra:
        raise ValueError(f"chunk {typ:#x} payload shorter than header")
    return u16(typ) + u16(header_size) + u32(8 + len(payload)) + payload


def utf16_pool(strings):
    offsets = []
    blob = bytearray()
    for s in strings:
        offsets.append(len(blob))
        raw = s.encode("utf-16le")
        blob += u16(len(s)) + raw + b"\x00\x00"
    while len(blob) % 4:
        blob += b"\x00"
    # header is 28 bytes; offsets follow; stringsStart points at the blob.
    strings_start = 28 + 4 * len(strings)
    header = (
        u32(len(strings))
        + u32(0)
        + u32(0)  # UTF-16
        + u32(strings_start)
        + u32(0)
    )
    payload = b"".join(u32(o) for o in offsets) + bytes(blob)
    return chunk(0x0001, 28, header + payload)


def build_binary_xml():
    strings = []

    def sid(value):
        if value not in strings:
            strings.append(value)
        return strings.index(value)

    # ("start"/"end", name, attrs)
    # attr: (ns or None, name, typed, value)
    nodes = [
        ("start", "manifest", [
            (None, "package", 0x03, "id.recomp.tarzan"),
            (ANDROID, "versionCode", 0x10, 1),
            (ANDROID, "versionName", 0x03, "0.1-experimental"),
        ]),
        ("start", "uses-sdk", [
            (ANDROID, "minSdkVersion", 0x10, 24),
            (ANDROID, "targetSdkVersion", 0x10, 24),
        ]),
        ("end", "uses-sdk", []),
        ("start", "uses-feature", [
            (ANDROID, "glEsVersion", 0x11, 0x00020000),
            (ANDROID, "required", 0x12, 0),
        ]),
        ("end", "uses-feature", []),
        ("start", "application", [
            (ANDROID, "label", 0x03, "Tarzan Recomp"),
            (ANDROID, "hasCode", 0x12, 0),
            (ANDROID, "debuggable", 0x12, 1),
        ]),
        ("start", "activity", [
            (ANDROID, "name", 0x03, "android.app.NativeActivity"),
            (ANDROID, "label", 0x03, "Tarzan Recomp"),
            (ANDROID, "configChanges", 0x03, "orientation|keyboardHidden|screenSize"),
            (ANDROID, "exported", 0x12, 1),
        ]),
        ("start", "meta-data", [
            (ANDROID, "name", 0x03, "android.app.lib_name"),
            (ANDROID, "value", 0x03, "tarzan"),
        ]),
        ("end", "meta-data", []),
        ("start", "intent-filter", []),
        ("start", "action", [(ANDROID, "name", 0x03, "android.intent.action.MAIN")]),
        ("end", "action", []),
        ("start", "category", [(ANDROID, "name", 0x03, "android.intent.category.LAUNCHER")]),
        ("end", "category", []),
        ("end", "intent-filter", []),
        ("end", "activity", []),
        ("end", "application", []),
        ("end", "manifest", []),
    ]
    sid(ANDROID)
    sid("android")
    for kind, name, attrs in nodes:
        sid(name)
        if kind != "start":
            continue
        for ns, aname, typed, value in attrs:
            if ns:
                sid(ns)
            sid(aname)
            if typed == 0x03:
                sid(value)

    pool = utf16_pool(strings)
    resmap = b"".join(u32(ATTR_ID.get(s, 0)) for s in strings)
    res_chunk = u16(0x0180) + u16(8) + u32(8 + len(resmap)) + resmap

    def node(typ, extra):
        # ResChunk_header is 8 bytes. headerSize 16 covers lineNumber and comment.
        # size is the whole chunk, including that 16-byte header and extra.
        body = u32(1) + u32(0xFFFFFFFF) + extra
        return u16(typ) + u16(16) + u32(8 + len(body)) + body

    xml = node(0x0100, u32(sid("android")) + u32(sid(ANDROID)))
    for kind, name, attrs in nodes:
        if kind == "end":
            xml += node(0x0103, u32(0xFFFFFFFF) + u32(sid(name)))
            continue
        attr_bytes = b""
        for ns, aname, typed, value in attrs:
            ns_i = sid(ns) if ns else 0xFFFFFFFF
            name_i = sid(aname)
            if typed == 0x03:
                raw = sid(value)
                data = raw
            else:
                raw = 0xFFFFFFFF
                data = value
            attr_bytes += u32(ns_i) + u32(name_i) + u32(raw) + u16(8) + b"\x00" + bytes([typed]) + u32(data)
        extra = (
            u32(0xFFFFFFFF)
            + u32(sid(name))
            + u16(20)
            + u16(20)
            + u16(len(attrs))
            + u16(0)
            + u16(0)
            + u16(0)
            + attr_bytes
        )
        xml += node(0x0102, extra)
    xml += node(0x0101, u32(sid("android")) + u32(sid(ANDROID)))
    file_body = pool + res_chunk + xml
    return u16(0x0003) + u16(8) + u32(8 + len(file_body)) + file_body


def ensure_key():
    if KEY.exists() and CERT.exists():
        key = serialization.load_pem_private_key(KEY.read_bytes(), password=None)
        cert = x509.load_pem_x509_certificate(CERT.read_bytes())
        return key, cert
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Tarzan Recomp Debug")])
    now = datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(1)
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=3650))
        .sign(key, hashes.SHA256())
    )
    KEY.parent.mkdir(parents=True, exist_ok=True)
    KEY.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    CERT.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    return key, cert


def der_len(n):
    if n < 128:
        return bytes([n])
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(raw)]) + raw


def der(tag, body):
    return bytes([tag]) + der_len(len(body)) + body


def read_tlv(buf, i):
    tag = buf[i]
    i += 1
    n = buf[i]
    i += 1
    if n & 0x80:
        ln = n & 0x7F
        n = int.from_bytes(buf[i : i + ln], "big")
        i += ln
    return tag, i, i + n


def issuer_and_serial(cert_der):
    _, i, end = read_tlv(cert_der, 0)
    tbs = cert_der[i:end]
    j = 0
    tag, start, nxt = read_tlv(tbs, j)
    if tag == 0xA0:
        j = nxt
        _, start, nxt = read_tlv(tbs, j)
    serial = tbs[j:nxt]
    j = nxt
    _, _, nxt = read_tlv(tbs, j)  # signature algorithm
    j = nxt
    issuer = tbs[j:read_tlv(tbs, j)[2]]
    return der(0x30, issuer + serial)


def pkcs7(sf, signature, cert_der):
    sha256 = der(0x30, der(0x06, bytes.fromhex("608648016503040201")) + der(0x05, b""))
    rsa_alg = der(0x30, der(0x06, bytes.fromhex("2a864886f70d010101")) + der(0x05, b""))
    signer = der(
        0x30,
        der(0x02, b"\x01")
        + issuer_and_serial(cert_der)
        + sha256
        + rsa_alg
        + der(0x04, signature),
    )
    content_info = der(0x06, bytes.fromhex("2a864886f70d010701")) + der(0xA0, der(0x04, sf))
    signed = der(
        0x30,
        der(0x02, b"\x01")
        + der(0x31, sha256)
        + der(0x30, content_info)
        + bytes([0xA0])
        + der_len(len(cert_der))
        + cert_der
        + der(0x31, signer),
    )
    return der(0x30, der(0x06, bytes.fromhex("2a864886f70d010702")) + der(0xA0, signed))


def jar_sign(entries, key, cert):
    lines = ["Manifest-Version: 1.0", "Created-By: tarzan-pack", ""]
    sections = []
    for name, data in entries:
        sec = f"Name: {name}\r\nSHA-256-Digest: {base64.b64encode(hashlib.sha256(data).digest()).decode()}\r\n\r\n"
        sections.append((name, sec.encode()))
    mf = ("\r\n".join(lines) + "\r\n").encode() + b"".join(sec for _, sec in sections)
    sf = [
        "Signature-Version: 1.0",
        "Created-By: tarzan-pack",
        "SHA-256-Digest-Manifest: " + base64.b64encode(hashlib.sha256(mf).digest()).decode(),
        "",
    ]
    for name, sec in sections:
        sf.append(f"Name: {name}")
        sf.append("SHA-256-Digest: " + base64.b64encode(hashlib.sha256(sec).digest()).decode())
        sf.append("")
    sf_bytes = ("\r\n".join(sf) + "\r\n").encode()
    signature = key.sign(sf_bytes, padding.PKCS1v15(), hashes.SHA256())
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    return mf, sf_bytes, pkcs7(sf_bytes, signature, cert_der)


def main():
    if not SO.is_file():
        print("missing", SO, file=sys.stderr)
        return 1
    blob = SO.read_bytes()
    if blob[:4] != b"\x7fELF" or blob[18] != 0xB7:
        print("libtarzan.so is not an AArch64 ELF", file=sys.stderr)
        return 1
    key, cert = ensure_key()
    xml = build_binary_xml()
    entries = [
        ("AndroidManifest.xml", xml),
        ("lib/arm64-v8a/libtarzan.so", blob),
    ]
    mf, sf, rsa_block = jar_sign(entries, key, cert)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(OUT, "w") as zf:
        for name, data in entries + [
            ("META-INF/MANIFEST.MF", mf),
            ("META-INF/CERT.SF", sf),
            ("META-INF/CERT.RSA", rsa_block),
        ]:
            info = zipfile.ZipInfo(name)
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            zf.writestr(info, data)
    names = zipfile.ZipFile(OUT).namelist()
    if any(n.endswith(".z64") for n in names):
        OUT.unlink()
        print("ROM leaked into APK", file=sys.stderr)
        return 1
    print(f"packed {OUT} ({OUT.stat().st_size} bytes)")
    print("entries", names)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
