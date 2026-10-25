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

Status: works in the emulator; not tested on a real Xtreme yet. The chat
bridge to the internet relay is not written yet.

## Build

```sh
git submodule update --init
make            # build/cywig (needs libusb-1.0 and curl)
make xtreme     # xtreme/USBGate.app (needs gcc-h8300-hms)
```

## Use

1. Copy `xtreme/USBGate.app` to the Xtreme, for example with EZ Loader.
2. Plug in the cable and run `build/cywig`. It starts USB Gate itself through
   the CyOS console when the app isn't running.
3. On another Cybiko, open the WAP browser or Browser.app.

To test without hardware, run the Dragonfruit emulator with
`cybiko_host --usb-server 5555 --install xtreme/USBGate.app ...`, then
`build/cywig --emulator 127.0.0.1:5555 -v`.

The USB protocol and the CyOS exports used here are described in
[cybiko-docs](https://github.com/SovGVD/cybiko-docs) `usb.md`.

## Credits

- Tim Schuerewegen: the MAME Cybiko driver, which documents the hardware
  (including the USBN9604 USB chip) that this work and the Dragonfruit emulator
  build on.
- The MAME team, for the H8S and Cybiko emulation the work started from.
- Cybiko Inc., for CyOS, the CyWIG and the original VCyWIG.
- The Cybiko community, for keeping the ROM and flash dumps, SDKs and CDs
  available.
