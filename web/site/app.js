/* The page: asks for the Xtreme (WebUSB wants a click), keeps the
 * settings, and runs the gate in gate-worker.js.
 * ?emulator=ws://127.0.0.1:8090 uses an emulated Xtreme instead (see
 * web/dev-bridge.mjs). */
"use strict";

const $ = id => document.getElementById(id);
const params = new URLSearchParams(location.search);
const emulator = params.get("emulator") || "";
const FIELDS = ["chat", "room", "nick", "home", "proxy", "verbose"];
let worker = null;

function log(line) {
    const el = $("log");
    const t = new Date().toTimeString().slice(0, 8);
    const atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 4;
    el.textContent += `${t} ${line}\n`;
    if (el.textContent.length > 200000) el.textContent = el.textContent.slice(-150000);
    if (atEnd) el.scrollTop = el.scrollHeight;
}

function state(text) { $("state").textContent = text; }

function load() {
    let s = {};
    try { s = JSON.parse(localStorage.getItem("cywig") || "{}"); } catch (e) { /* none saved */ }
    for (const f of FIELDS) {
        if (!(f in s)) continue;
        if ($(f).type === "checkbox") $(f).checked = !!s[f]; else $(f).value = s[f];
    }
}

function settings() {
    const s = {};
    for (const f of FIELDS) s[f] = $(f).type === "checkbox" ? $(f).checked : $(f).value.trim();
    try { localStorage.setItem("cywig", JSON.stringify(s)); } catch (e) { /* not kept */ }
    s.emulator = emulator;
    return s;
}

function running(on) {
    $("connect").hidden = on;
    $("stop").hidden = !on;
    for (const f of FIELDS) $(f).disabled = on;
    $("chatnow").disabled = !on || !$("chat").checked;
    $("chatnow").checked = on && $("chat").checked;
}

async function connect() {
    const s = settings();
    if (!emulator) {
        try {
            await navigator.usb.requestDevice({ filters: [{ vendorId: 0x0b66, productId: 0x0041 }] });
        } catch (e) {
            log(`usb: no Xtreme picked (${e.message})`);
            return;
        }
    }
    state("Starting…");
    worker = new Worker("gate-worker.js");
    worker.onmessage = e => {
        const m = e.data;
        if (m.t === "log") log(m.line);
        else if (m.t === "running") { state("Running"); running(true); }
        else if (m.t === "relay") log(m.online ? "chat: on the Internet" : "chat: off the Internet");
        else if (m.t === "stopped") {
            state(m.why ? `Stopped: ${m.why}` : "Stopped");
            running(false);
            worker.terminate();
            worker = null;
        }
    };
    worker.onerror = e => { log(`gate: ${e.message}`); state("Stopped"); running(false); };
    worker.postMessage({ t: "start", settings: s });
}

$("connect").onclick = connect;
$("stop").onclick = () => { if (worker) worker.postMessage({ t: "stop" }); state("Stopping…"); };
$("chatnow").onchange = () => { if (worker) worker.postMessage({ t: "chat", on: $("chatnow").checked }); };
load();
if (!("usb" in navigator) && !emulator) {
    $("nousb").hidden = false;
    $("connect").disabled = true;
}
if (emulator) log(`using the emulated Xtreme at ${emulator}`);
navigator.usb && navigator.usb.addEventListener("disconnect", e => {
    if (e.device.vendorId === 0x0b66) log("usb: the Xtreme was unplugged");
});
