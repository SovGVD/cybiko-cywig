# cybiko-cywig

A virtual CyWIG (Cybiko Wireless Internet Gateway) for a stock **Cybiko Xtreme**
on its USB cable. It needs no ROM or flash changes.

- `xtreme/`: **USB Gate** (`USBGate.app`), a normal CyOS app. It switches the
  Xtreme into gate mode, so other Cybikos in radio range see it as a CyWIG. It
  passes their messages for port 93 to the PC over a CyOS USB pipe (logical
  pipe 40), and sends the PC's answers back over the radio.
- `pc/`: **cywig**, the PC side. It speaks the CyOS Multi Channel Protocol
  (MCP) over USB and runs the gate from
  [cybiko-browser](https://github.com/SovGVD/cybiko-browser) (`cy_gate`). It
  fetches pages and turns them into WML for the WAP browser, or into laid-out
  pages for Browser.app.
- `web/`: the same cywig as a **web page**, nothing to install: Chrome or
  Edge talk to the Xtreme with WebUSB, see [In a web browser](#in-a-web-browser).
- **Chat over the Internet** (`cywig --chat`): the Xtreme also passes on
  the Chat lines it hears, and cywig joins a room of the radio relay that the
  [Cybiko web emulator](https://github.com/SovGVD/cybiko-web-emulator) uses
  (`cybiko-relay.sovgvd.workers.dev`, room `public`). Lines from the Cybikos
  around show up in the web emulators as those Cybikos' own (`@ID`), and the
  room's lines reach the Cybikos around from the Xtreme, with the sender's
  nickname in front ("Bob: hi"). Type `chat off` / `chat on` while cywig runs
  to switch it.

Status: pages and chat both ways work in the emulator, from cywig and from
the web page in headless Chrome (a radio Cybiko, the
emulated Xtreme on the cable, and a local copy of the relay). Not tested on a
real Xtreme yet, and the real Cloudflare relay wasn't reachable from where it
was built.

## Build

```sh
git submodule update --init
make            # build/cywig and build/chat_peer (needs libusb-1.0, OpenSSL and curl)
make xtreme     # xtreme/USBGate.app (needs gcc-h8300-hms)
```

`make NET=libcurl` fetches pages with libcurl instead of running the curl
program. On Windows, build in an [MSYS2](https://www.msys2.org/) UCRT64 shell
with `pacman -S make mingw-w64-ucrt-x86_64-{gcc,pkgconf,libusb,openssl,curl}`
and `make` (libcurl is the default there); the exe needs the DLLs from
`/ucrt64/bin` next to it, and the Xtreme needs the WinUSB driver (see
[In a web browser](#in-a-web-browser)).

GitHub Actions builds all of it on every push (`.github/workflows/build.yml`):
the run's artifacts are `cywig-linux-x86_64`, `cywig-windows-x86_64`
(cywig.exe with its DLLs and USBGate.app) and `USBGate.app`.

## Use

1. Copy `xtreme/USBGate.app` to the Xtreme, for example with EZ Loader.
2. Plug in the cable and run `build/cywig`. It starts USB Gate itself through
   the CyOS console when the app isn't running.
3. On another Cybiko, open the WAP browser or Browser.app.

Options: `--chat` (bridge Chat), `--room NAME`, `--relay URL`, `--nick NAME`
(the bridge's name in the room), `--home URL` (the WAP browser's Home),
`--flash FILE` (a CyOS flash image, for the fonts Browser.app pages are laid
out with), `-v` (log every message).

To test without hardware, run the Dragonfruit emulator with
`cybiko_host --usb-server 5555 --install xtreme/USBGate.app --bot Bob --bot-get 35000:URL --bot-say 40000:hi`,
then `build/cywig --emulator 127.0.0.1:5555 --chat -v`. `build/chat_peer
--nick Ann --say 5000:hello` sits in a relay room as one more Cybiko and
prints what it hears; `node proxy/local.mjs` from the web emulator is a relay
on your own computer (`--relay http://127.0.0.1:8080`).

## In a web browser

<https://sovgvd.github.io/cybiko-cywig/> runs cywig in the page (WebAssembly
in a worker, so it keeps going in a background tab). Copy `USBGate.app` to
the Xtreme (the page has a link to it), plug it in, press **Connect** and
pick the Cybiko. The settings (Chat, room, nickname, Home page) are under
Settings; fetching and Chat go through the web emulator's Cloudflare Worker.

It needs a browser with WebUSB: Chrome, Edge or another Chromium, on a
computer or on Android with an OTG cable (not Firefox or Safari).

- **Windows**: WebUSB can only open a device that has the WinUSB driver.
  Install it once with [Zadig](https://zadig.akeo.ie/): Options -> List All
  Devices, pick the Cybiko (USB ID `0B66 0041`), choose WinUSB, Install
  Driver. (EZ Loader uses its own driver, so to use EZ Loader again, put that
  one back in Device Manager.)
- **Linux**: allow your user to open it, e.g. `/etc/udev/rules.d/50-cybiko.rules`
  with `SUBSYSTEM=="usb", ATTR{idVendor}=="0b66", MODE="0666"`, then plug it in again.
- **macOS, ChromeOS, Android**: nothing to set up.

Build it with Emscripten (`source emsdk_env.sh`, then `sh web/build.sh`;
the Pages workflow does that on every push to main) and serve `web/site/`
over https or from localhost. To try it without an Xtreme, run
`cybiko_host --usb-server 5555 --install xtreme/USBGate.app ...` as below,
`node web/dev-bridge.mjs` (it serves the page and passes a WebSocket to the
emulator), and open
`http://localhost:8090/?emulator=ws://localhost:8090/usb`.

How USB Gate does it on a stock Xtreme (CyOS 1.5.08 only, it uses kernel
exports and two kernel structures directly):

- gate mode on (export 1152), so CyOS marks everything it sends as a gate's;
- a ping every 3 s on the base channel (export 1079), as an idle Xtreme
  never pings and nobody would find the gate;
- a queue for port 93 (a process made with the port as its name, never
  started) and the MCP pipe 40 to the PC (exports 1680, 1604-1628);
- with Chat bridged: the radio on RF channel 4 (export 1161), and the chat
  lines taken from the queue a resident part of CyOS keeps for port 25
  (only messages 1002; it is found in CyOS's ring of port queues).

The USB protocol and the CyOS exports used here are described in
[cybiko-docs](https://github.com/SovGVD/cybiko-docs) `usb.md`.

## Credits

- `pc/cy_radio` is Dragonfruit's radio protocol library; the protocol was
  worked out from CyOS's code and traces (cybiko-docs `radio-protocol.md`).
- Tim Schuerewegen: the MAME Cybiko driver, which documents the hardware
  (including the USBN9604 USB chip) that this work and the Dragonfruit emulator
  build on.
- The MAME team, for the H8S and Cybiko emulation the work started from.
- Cybiko Inc., for CyOS, the CyWIG and the original VCyWIG.
- The Cybiko community, for keeping the ROM and flash dumps, SDKs and CDs
  available.
