/* Runs the gate (cywig.wasm) in a worker, so that a tab in the background
 * keeps it going. The page asked for the Xtreme (WebUSB needs a click
 * there); here it is opened again from the devices the page may use.
 *
 * Module.usb: the cable, as WebUSB or, for trying it without an Xtreme, a
 * WebSocket to Dragonfruit's emulator (cybiko_host --usb-server, through
 * web/dev-bridge.mjs). USB Gate takes the Xtreme off the bus for a moment
 * when it starts and stops, and it comes back as another USB device
 * (0B66:0042, which Windows gives WinUSB by itself), so the cable is
 * opened again (cable.open) whenever the gate finds the old one gone. Module.net: fetches through the proxy (the web
 * emulator's Cloudflare Worker). Module.relay: the chat room's WebSocket. */

let settings = {};
const log = line => postMessage({ t: "log", line });

function timeout(ms) {
    return new Promise(res => setTimeout(() => res("timeout"), Math.max(1, ms)));
}

/* ---- the cable over WebUSB ---- */

/* USB Gate running first: it's the one Windows can open */
const XTREME = [0x0042, 0x0041];

async function openWebusb() {
    const devs = (await navigator.usb.getDevices()).filter(d => d.vendorId === 0x0b66 && XTREME.includes(d.productId));
    devs.sort((a, b) => XTREME.indexOf(a.productId) - XTREME.indexOf(b.productId));
    if (!devs.length) return null;
    const dev = devs[0];
    await dev.open();
    if (!dev.configuration) await dev.selectConfiguration(1);
    await dev.claimInterface(0);
    log(`usb: ${dev.productName || "Cybiko"} by ${dev.manufacturerName || "?"}, ` +
        `0b66:${dev.productId.toString(16).padStart(4, "0")}${dev.productId === 0x0042 ? " (USB Gate running)" : ""}`);
    return webusb(dev);
}

function webusb(dev) {
    const pendingIn = {};          /* endpoint -> a transferIn that hasn't come back yet */
    const leftover = {};           /* endpoint -> bytes that came and weren't asked for yet */
    let pendingOut = null;
    const setup = (type, req, value, index) => ({
        requestType: ["standard", "class", "vendor", "standard"][(type >> 5) & 3],
        recipient: ["device", "interface", "endpoint", "other"][Math.min(type & 0x1f, 3)],
        request: req, value, index,
    });
    return {
        async control(type, req, value, index, len, out, ms) {
            try {
                const p = type & 0x80 ? dev.controlTransferIn(setup(type, req, value, index), len)
                                      : dev.controlTransferOut(setup(type, req, value, index), out);
                const r = await Promise.race([p, timeout(ms)]);
                if (r === "timeout") return { result: -1 };
                if (r.status === "stall") return { result: -2 };
                if (type & 0x80) return { result: r.data.byteLength, data: new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength) };
                return { result: r.bytesWritten };
            } catch (e) { log(`usb: control transfer failed: ${e.message}`); return { result: -3 }; }
        },
        async bulk(ep, len, out, ms) {
            const n = ep & 0x7f;
            try {
                if (ep & 0x80) {
                    if (leftover[n] && leftover[n].length) {
                        const d = leftover[n].subarray(0, len);
                        leftover[n] = leftover[n].subarray(d.length);
                        return { result: d.length, data: d };
                    }
                    if (!pendingIn[n]) pendingIn[n] = dev.transferIn(n, 4096);
                    const r = await Promise.race([pendingIn[n], timeout(ms)]);
                    if (r === "timeout") return { result: 0, timedOut: true };
                    pendingIn[n] = null;
                    if (r.status === "stall") { await dev.clearHalt("in", n); return { result: -2 }; }
                    const all = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
                    leftover[n] = all.subarray(Math.min(len, all.length));
                    return { result: Math.min(len, all.length), data: all.subarray(0, len) };
                }
                if (pendingOut) {                         /* the last one is still on its way */
                    const r = await Promise.race([pendingOut, timeout(ms)]);
                    if (r === "timeout") return { result: 0, timedOut: true };
                    pendingOut = null;
                }
                pendingOut = dev.transferOut(n, out);
                const r = await Promise.race([pendingOut, timeout(ms)]);
                if (r === "timeout") return { result: 0, timedOut: true };
                pendingOut = null;
                if (r.status === "stall") { await dev.clearHalt("out", n); return { result: -2 }; }
                return { result: r.bytesWritten };
            } catch (e) {
                log(`usb: bulk transfer failed: ${e.message}`);
                return { result: -3 };
            }
        },
    };
}

/* ---- the cable to an emulated Xtreme ----
 * cybiko-browser usb/cyusb_tcp.h: 'C' type req value(2) index(2) len(2)
 * timeout(4) [data] -> result(4) [data]; 'B' ep len(4) timeout(4) [data] ->
 * result(4) timed_out(1) [data]. One request at a time. */
function emulated(url) {
    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    let buf = new Uint8Array(0), want = null, chain = Promise.resolve();
    const opened = new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error(`can't reach ${url}`)); });
    ws.onmessage = e => {
        const d = new Uint8Array(e.data), b = new Uint8Array(buf.length + d.length);
        b.set(buf); b.set(d, buf.length); buf = b;
        if (want) want();
    };
    ws.onclose = () => { log("usb: the emulator went"); if (want) want(); };
    const take = n => new Promise(res => {
        const check = () => {
            if (buf.length >= n) { const d = buf.slice(0, n); buf = buf.slice(n); want = null; res(d); }
            else if (ws.readyState > 1) { want = null; res(null); }
        };
        want = check; check();
    });
    const be = (n, v) => { const a = []; for (let i = n - 1; i >= 0; i--) a.push((v >>> (8 * i)) & 255); return a; };
    const num = d => ((d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3]) | 0;
    const one = f => (chain = chain.then(f, f));
    return {
        opened,
        control: (type, req, value, index, len, out, ms) => one(async () => {
            ws.send(new Uint8Array(["C".charCodeAt(0), type, req, ...be(2, value), ...be(2, index), ...be(2, len), ...be(4, ms),
                                    ...(out ? out : [])]));
            const h = await take(4);
            if (!h) return { result: -3 };
            const r = num(h);
            const data = type & 0x80 && r > 0 ? await take(r) : null;
            return { result: r, data };
        }),
        bulk: (ep, len, out, ms) => one(async () => {
            ws.send(new Uint8Array(["B".charCodeAt(0), ep, ...be(4, len), ...be(4, ms), ...(out ? out : [])]));
            const h = await take(5);
            if (!h) return { result: -3 };
            const r = num(h);
            const data = ep & 0x80 && r > 0 ? await take(r) : null;
            return { result: r, timedOut: !!h[4], data };
        }),
    };
}

/* ---- the cable, whichever it is, found again when it goes ---- */
const cable = {
    t: null, fresh: false, stopping: false, asked: false,
    async start() {
        this.t = settings.emulator ? await openEmulated() : await openWebusb();
        if (!this.t) throw new Error("the Xtreme isn't there (plugged out, or CyOS not running?)");
        this.fresh = true;
    },
    /* for the gate (usb_web.c): true once there is a cable, false when stopping */
    async open() {
        if (this.fresh) { this.fresh = false; return true; }
        this.t = null;
        for (let i = 0; !this.stopping; i++) {
            try { this.t = settings.emulator ? await openEmulated() : await openWebusb(); }
            catch (e) { if (i % 20 === 0) log(`usb: ${e.message}`); }
            if (this.t) { postMessage({ t: "found" }); return true; }
            /* a USB device the page hasn't been allowed to use yet (0042
             * the first time): only a click on the page can allow it */
            if (!settings.emulator && i === 6 && !this.asked) { this.asked = true; postMessage({ t: "pick" }); }
            await timeout(500);
        }
        return false;
    },
    control(...a) { return this.t ? this.t.control(...a) : Promise.resolve({ result: -3 }); },
    bulk(...a) { return this.t ? this.t.bulk(...a) : Promise.resolve({ result: -3 }); },
};

async function openEmulated() {
    const u = emulated(settings.emulator);
    await u.opened;
    log(`usb: emulated Xtreme at ${settings.emulator}`);
    return u;
}

/* ---- the gate's network: one hop at a time through the proxy ----
 * (the same as the web emulator's, see net_web.c) */
const net = {
    req: null,
    start(url, body, ctype, cookie) {
        this.abort();
        const proxy = (settings.proxy || "").replace(/\/+$/, "");
        const r = { state: 0, head: {}, chunks: [], done: false, error: "", ctl: new AbortController() };
        this.req = r;
        if (!proxy) { r.error = "No proxy set"; return false; }
        if (!/^https?:\/\//i.test(url)) { r.error = "Only http and https"; return false; }
        const headers = { "X-Cookie": cookie || "" };
        if (body !== null) headers["Content-Type"] = ctype || "application/x-www-form-urlencoded";
        fetch(proxy + "/fetch?url=" + encodeURIComponent(url), {
            method: body !== null ? "POST" : "GET", body: body !== null ? body : undefined,
            headers, signal: r.ctl.signal,
        }).then(async res => {
            if (this.req !== r) return;
            const h = res.headers;
            if (!res.ok || !h.get("X-Status")) {
                r.error = h.get("X-Error") || `Proxy said ${res.status}`;
                r.state = -2;
                log(`net: ${url}: ${r.error}`);
                return;
            }
            let cookies = [];
            try { cookies = JSON.parse(decodeURIComponent(h.get("X-Set-Cookie") || "%5B%5D")); }
            catch (e) { log(`net: bad X-Set-Cookie from the proxy: ${e}`); }
            r.head = { status: h.get("X-Status"), location: h.get("X-Location") || "",
                       type: h.get("X-Content-Type") || "", url: h.get("X-Final-Url") || url };
            cookies.forEach((c, i) => { r.head["cookie" + i] = c; });
            r.state = 1;
            const reader = res.body.getReader();
            for (;;) {
                const { done, value } = await reader.read();
                if (this.req !== r) { reader.cancel(); return; }
                if (done) break;
                r.chunks.push(value);
            }
            r.done = true;
        }).catch(e => {
            if (this.req !== r || e.name === "AbortError") return;
            r.error = `Can't reach the proxy (${e.message})`;
            if (r.state === 1) r.failed = true; else r.state = -2;
            log(`net: ${url}: ${r.error}`);
        });
        return true;
    },
    poll() { return this.req ? this.req.state : -2; },
    get(what) {
        const r = this.req;
        if (!r) return "";
        if (what === "error") return r.error;
        return r.head[what] || "";
    },
    read(cap) {
        const r = this.req;
        if (!r) return -2;
        if (r.chunks.length) {
            const c = r.chunks[0];
            if (c.length <= cap) { r.chunks.shift(); return c; }
            r.chunks[0] = c.subarray(cap);
            return c.subarray(0, cap);
        }
        if (r.failed) return -2;
        return r.done ? -1 : 0;
    },
    abort() {
        if (this.req) this.req.ctl.abort();
        this.req = null;
    },
};

/* ---- the chat room ---- */
const relay = {
    ws: null, url: "", inbox: [], retry: 0, timer: 0,
    open(url) { this.url = url; this.connect(); },
    connect() {
        const s = new WebSocket(this.url);
        s.binaryType = "arraybuffer";
        this.ws = s;
        s.onopen = () => { this.retry = 0; postMessage({ t: "relay", online: true }); };
        s.onmessage = e => {
            if (this.inbox.length > 256) { log("relay: too much at once, a message dropped"); return; }
            this.inbox.push(typeof e.data === "string" ? e.data : new Uint8Array(e.data));
        };
        s.onclose = e => {
            if (this.ws !== s) return;
            this.ws = null;
            postMessage({ t: "relay", online: false });
            const wait = Math.min(30000, 1000 * 2 ** this.retry++);
            log(`relay: connection closed (${e.code}); again in ${wait / 1000} s`);
            this.timer = setTimeout(() => { if (this.url) this.connect(); }, wait);
        };
    },
    close() { this.url = ""; clearTimeout(this.timer); if (this.ws) { const s = this.ws; this.ws = null; s.close(); } },
    online() { return !!this.ws && this.ws.readyState === 1; },
    send(d) { if (this.online()) this.ws.send(d); },
    next() { return this.inbox.shift() || null; },
};

/* ---- start ---- */

let M = null;
onmessage = async e => {
    const m = e.data;
    if (m.t === "start") {
        settings = m.settings;
        try {
            await cable.start();
        } catch (err) {
            log(`usb: ${err.message}`);
            postMessage({ t: "stopped", why: err.message });
            return;
        }
        M = await CywigModule({
            usb: cable, net, relay,
            log,
            done: r => postMessage({ t: "stopped", why: r ? "the gate stopped (see the log)" : "" }),
            print: log,
            printErr: log,
        });
        postMessage({ t: "running" });
        const args = [];
        if (settings.home) args.push("--home", settings.home);
        if (settings.chat) args.push("--chat", "--room", settings.room || "public", "--relay", settings.proxy, "--nick", settings.nick || "USBGate");
        if (settings.verbose) args.push("-v");
        try { M.callMain(args); } catch (err) { log(`gate: ${err.message || err}`); }
    } else if (m.t === "chat" && M) {
        M._cywig_chat(m.on ? 1 : 0);
    } else if (m.t === "stop" && M) {
        cable.stopping = true;
        M._cywig_stop();
    }
};

importScripts("cywig.js");
