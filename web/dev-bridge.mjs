#!/usr/bin/env node
/* For trying the web gate without an Xtreme: serves web/site/ and passes a
 * WebSocket on /usb to Dragonfruit's emulated Xtreme
 * (cybiko_host --usb-server PORT). Needs Node 18 or newer, no packages.
 *
 *   node web/dev-bridge.mjs [--port 8090] [--usb 127.0.0.1:5555]
 *
 * then open http://localhost:8090/?emulator=ws://localhost:8090/usb */
import http from "node:http";
import net from "node:net";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const SITE = path.join(here, "site");
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const PORT = +opt("--port", 8090);
const [UHOST, UPORT] = opt("--usb", "127.0.0.1:5555").split(":");
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm" };
const log = line => console.log(`${new Date().toISOString().slice(11, 19)} ${line}`);

const server = http.createServer((req, res) => {
    const p = new URL(req.url, "http://x").pathname;
    const file = path.resolve(SITE, "." + (p === "/" ? "/index.html" : p));
    if (!file.startsWith(SITE + path.sep)) { res.writeHead(403).end(); return; }
    fs.readFile(file, (err, data) => {
        if (err) { res.writeHead(404).end("not found"); return; }
        res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream" }).end(data);
    });
});

function wsSend(sock, data) {
    const n = data.length;
    const head = n < 126 ? Buffer.from([0x82, n]) : Buffer.from([0x82, 126, n >> 8, n & 255]);
    sock.write(Buffer.concat([head, data]));
}

server.on("upgrade", (req, sock) => {
    if (new URL(req.url, "http://x").pathname !== "/usb") { sock.end("HTTP/1.1 404 Not Found\r\n\r\n"); return; }
    const accept = crypto.createHash("sha1").update(req.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
    sock.setNoDelay(true);
    sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
    const usb = net.connect(+UPORT, UHOST, () => log(`usb: connected to the emulator at ${UHOST}:${UPORT}`));
    usb.setNoDelay(true);
    usb.on("data", d => { for (let i = 0; i < d.length; i += 65000) wsSend(sock, d.subarray(i, i + 65000)); });
    usb.on("error", e => { log(`usb: ${e.message}`); sock.destroy(); });
    usb.on("close", () => sock.destroy());
    let buf = Buffer.alloc(0);
    sock.on("data", d => {
        buf = Buffer.concat([buf, d]);
        for (;;) {
            if (buf.length < 2) return;
            let len = buf[1] & 127, off = 2;
            if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
            else if (len === 127) { log("ws: frame too big"); sock.destroy(); return; }
            const masked = buf[1] & 128;
            if (buf.length < off + (masked ? 4 : 0) + len) return;
            const mask = masked ? buf.subarray(off, off + 4) : null;
            off += masked ? 4 : 0;
            const data = Buffer.from(buf.subarray(off, off + len));
            if (mask) for (let i = 0; i < len; i++) data[i] ^= mask[i & 3];
            const op = buf[0] & 15;
            buf = buf.subarray(off + len);
            if (op === 8) { sock.end(); usb.end(); return; }
            if (op === 2 || op === 0) usb.write(data);
        }
    });
    sock.on("close", () => { usb.destroy(); log("usb: the page went"); });
    sock.on("error", () => usb.destroy());
});

server.listen(PORT, () => log(`web gate on http://localhost:${PORT}/?emulator=ws://localhost:${PORT}/usb`));
