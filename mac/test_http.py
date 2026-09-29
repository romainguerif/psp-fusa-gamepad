"""End-to-end test of pspbridged against the fake PSP (a folder playing ms0:/).

Builds a real ISO9660 image with a PSP_GAME/PARAM.SFO (hdiutil makehybrid,
command line only), serves it, and checks what PPSSPP's Remote ISO relies
on: HEAD with Content-Length and Accept-Ranges, byte ranges, keep-alive.
"""

import http.client
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
DAEMON = os.path.join(HERE, "build", "pspbridged")


def make_sfo(entries):
    """entries: list of (key, utf8 string value)"""
    entries = sorted(entries)
    keys = b""
    data = b""
    index = b""
    for key, value in entries:
        raw = value.encode("utf-8") + b"\0"
        maxlen = (len(raw) + 3) & ~3
        index += struct.pack("<HHIII", len(keys), 0x0204, len(raw), maxlen, len(data))
        keys += key.encode() + b"\0"
        data += raw + b"\0" * (maxlen - len(raw))
    while len(keys) % 4:
        keys += b"\0"
    key_start = 20 + len(index)
    data_start = key_start + len(keys)
    return struct.pack("<4sIIII", b"\0PSF", 0x0101, key_start, data_start, len(entries)) + index + keys + data


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class HttpTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp()
        root = os.path.join(cls.work, "ms0")
        iso_dir = os.path.join(root, "ISO")
        os.makedirs(iso_dir)
        # the game: PARAM.SFO + a big EBOOT-like file so the ISO spans many blocks
        src = os.path.join(cls.work, "disc")
        os.makedirs(os.path.join(src, "PSP_GAME", "SYSDIR"))
        with open(os.path.join(src, "PSP_GAME", "PARAM.SFO"), "wb") as f:
            f.write(make_sfo([("TITLE", "Crème Test \"Racer\""), ("DISC_ID", "ULES00123"),
                              ("CATEGORY", "UG")]))
        with open(os.path.join(src, "PSP_GAME", "SYSDIR", "EBOOT.BIN"), "wb") as f:
            f.write(os.urandom(700 * 1024 + 17))
        iso = os.path.join(iso_dir, "Test Racer.iso")
        subprocess.check_call(["hdiutil", "makehybrid", "-quiet", "-iso", "-o", iso, src])
        with open(iso, "rb") as f:
            cls.iso = f.read()
        # things that are not ISOs are not listed
        open(os.path.join(iso_dir, "notes.txt"), "w").write("x")
        os.makedirs(os.path.join(iso_dir, "folder.iso"))
        # a second ISO without PSP_GAME: listed, no title
        with open(os.path.join(iso_dir, "blank.ISO"), "wb") as f:
            f.write(b"\0" * 40000)

        cls.port = free_port()
        cls.proc = subprocess.Popen([DAEMON, "--fake", root, "--port", str(cls.port)],
                                    stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", cls.port), timeout=0.1).close()
                break
            except OSError:
                time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        shutil.rmtree(cls.work, ignore_errors=True)

    def request(self, method, path, headers=None, conn=None):
        c = conn or http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request(method, path, headers=headers or {})
        r = c.getresponse()
        body = r.read()
        if conn is None:
            c.close()
        return r, body

    def test_status(self):
        r, body = self.request("GET", "/status")
        self.assertEqual(r.status, 200)
        st = json.loads(body)
        self.assertTrue(st["connected"])
        self.assertEqual(st["protocol"], 2)

    def test_list(self):
        r, body = self.request("GET", "/iso")
        self.assertEqual(r.status, 200)
        games = json.loads(body)
        self.assertEqual([g["name"] for g in games], ["blank.ISO", "Test Racer.iso"])
        racer = games[1]
        self.assertEqual(racer["size"], len(self.iso))
        self.assertEqual(racer["title"], "Crème Test \"Racer\"")
        self.assertEqual(racer["id"], "ULES00123")
        self.assertEqual(racer["url"], "/iso/Test%20Racer.iso")
        self.assertEqual(games[0]["title"], "")

    def test_root_listing_for_ppsspp(self):
        # PPSSPP's Remote tab: text/plain, one path per line, games only
        r, body = self.request("GET", "/", {"Accept": "text/plain, text/html; q=0.9, */*; q=0.8"})
        self.assertEqual(r.status, 200)
        self.assertTrue(r.getheader("Content-Type").startswith("text/plain"))
        self.assertEqual(body.decode().splitlines(), ["/iso/blank.ISO", "/iso/Test Racer.iso"])
        # what PPSSPP 1.20 really sends: base URL + line = "//iso/...", with
        # the spaces not encoded
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        s.sendall(b"GET //iso/Test Racer.iso HTTP/1.1\r\nHost: x\r\nRange: bytes=0-99\r\n"
                  b"Connection: close\r\n\r\n")
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        s.close()
        head, _, body = data.partition(b"\r\n\r\n")
        self.assertIn(b"206", head.split(b"\r\n")[0])
        self.assertEqual(body, self.iso[:100])

    def test_head(self):
        r, body = self.request("HEAD", "/iso/Test%20Racer.iso")
        self.assertEqual(r.status, 200)
        self.assertEqual(int(r.getheader("Content-Length")), len(self.iso))
        self.assertEqual(r.getheader("Accept-Ranges"), "bytes")
        self.assertEqual(body, b"")

    def test_ranges(self):
        n = len(self.iso)
        cases = [(0, 2047), (32768, 34815), (65530, 65545), (100000, 300000),
                 (n - 10, n - 1), (n - 10, n + 500)]
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        for a, b in cases:  # all on one keep-alive connection, like PPSSPP
            r, body = self.request("GET", "/iso/Test%20Racer.iso",
                                   {"Range": "bytes=%d-%d" % (a, b)}, conn=c)
            self.assertEqual(r.status, 206, (a, b))
            last = min(b, n - 1)
            self.assertEqual(body, self.iso[a:last + 1], (a, b))
            self.assertEqual(r.getheader("Content-Range"), "bytes %d-%d/%d" % (a, last, n))
        r, body = self.request("GET", "/iso/Test%20Racer.iso", {"Range": "bytes=-100"}, conn=c)
        self.assertEqual(body, self.iso[-100:])
        r, body = self.request("GET", "/iso/Test%20Racer.iso", {"Range": "bytes=1000-"}, conn=c)
        self.assertEqual(body, self.iso[1000:])
        c.close()

    def test_whole_file(self):
        r, body = self.request("GET", "/iso/Test%20Racer.iso")
        self.assertEqual(r.status, 200)
        self.assertEqual(body, self.iso)

    def test_errors(self):
        r, _ = self.request("GET", "/iso/Test%20Racer.iso",
                            {"Range": "bytes=%d-" % (len(self.iso) + 1)})
        self.assertEqual(r.status, 416)
        for path in ("/iso/missing.iso", "/iso/notes.txt", "/iso/folder.iso",
                     "/iso/..%2F..%2Fetc%2Fpasswd.iso", "/iso/a%2Fb.iso", "/nothing"):
            r, _ = self.request("GET", path)
            self.assertEqual(r.status, 404, path)
        r, _ = self.request("POST", "/iso")
        self.assertEqual(r.status, 405)


if __name__ == "__main__":
    unittest.main(verbosity=1)
