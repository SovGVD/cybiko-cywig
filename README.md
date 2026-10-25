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

When USB Gate starts, the Xtreme leaves USB for half a second and comes
back as `0B66 0042` (and as `0041` again when it stops). The page and
cywig find it again by themselves; the first time, the browser needs a
click to allow the new device, so the page shows **Pick it again**.

- **Windows** (8 or newer): start USB Gate on the Xtreme by hand, give
  Windows a few seconds the first time, then press Connect and pick the
  Cybiko. While USB Gate runs, the Xtreme is a USB device of its own
  (`0B66 0042`) that asks Windows for the WinUSB driver itself (Microsoft OS
  descriptors), so there is nothing to install; see
  [WinUSB without Zadig](#winusb-without-zadig). Without USB Gate the Xtreme
  is `0B66 0041`, which Windows only opens after installing WinUSB with
  [Zadig](https://zadig.akeo.ie/) (Options -> List All Devices, the Cybiko,
  WinUSB, Install Driver); that is only needed for the page to start USB Gate
  itself. EZ Loader's own driver stays on `0041`, so EZ Loader keeps
  working.
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

## WinUSB without Zadig

Windows binds a driver by itself to a vendor class device only when the
device asks for one with Microsoft OS 1.0 descriptors ("WCID"). CyOS 1.5.08
has none, so while it runs USB Gate adds them, in RAM only:

- It puts a hook in CyOS's USB control request handler (kernel code in RAM
  at 4accb2) that answers string descriptor `ee` (`MSFT100`, vendor request
  `47`), the compatible ID request (`WINUSB`) and the extended properties
  request (`DeviceInterfaceGUID` =
  `{A81E3782-6DF8-4A47-8F65-692FACB7C0DB}`), and passes every other request
  to CyOS.
- It changes CyOS's device descriptor to USB 2.0 (Windows only asks USB 2.0
  devices; still full speed, and the hook stalls the high speed descriptor
  requests as a full speed USB 2.0 device must) and product ID `0042`.
  Windows remembers per USB ID whether a device has the descriptors, and a
  driver installed for `0041` (EZ Loader's, or Zadig's) would win anyway.
- It drops off the bus for 400 ms (USBN9604 MCNTRL.NAT) so the PC reads the
  new descriptors, and on the way out puts everything back and does it
  again. A restart of the Xtreme undoes it as well.
- It checks the kernel's bytes first; on another CyOS it does none of this
  (cywig then logs that the Xtreme stayed `0041`).

On a PC that never saw `0B66 0042`, Windows reads the descriptors once and
installs WinUSB from its own driver store. Linux and macOS don't care, and
cywig and the page open either ID. Verified only in Dragonfruit's emulator,
whose USB host enumerates like Windows and logs the descriptors (the
`--usb` script step `reenum` waits for a drop-off and enumerates again).

## Credits

- `pc/cy_radio` is Dragonfruit's radio protocol library; the protocol was
  worked out from CyOS's code and traces (cybiko-docs `radio-protocol.md`).
- Tim Schuerewegen: the MAME Cybiko driver, which documents the hardware
  (including the USBN9604 USB chip) that this work and the Dragonfruit emulator
  build on.
- The MAME team, for the H8S and Cybiko emulation the work started from.
- Cybiko Inc., for CyOS, the CyWIG and the original VCyWIG.
- Pete Batard (libwdi, Zadig): the
  [WCID devices](https://github.com/pbatard/libwdi/wiki/WCID-Devices) notes the
  WinUSB descriptors follow.
- The Cybiko community, for keeping the ROM and flash dumps, SDKs and CDs
  available.
