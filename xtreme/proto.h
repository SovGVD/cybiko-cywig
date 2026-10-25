/* What the USB Gate application on the Xtreme and the PC say to each other
 * over the USB cable: records on CyOS's logical pipe UG_PIPE (MCP pipes
 * 2N-1 from the PC, 2N to it, see usb/cymcp.h). Only #defines, so both the
 * H8S application and the PC code include it.
 *
 * Every record: type (1 byte), 0, payload length (2 bytes), payload.
 * Numbers are big-endian, like everything on a Cybiko.
 *
 * From the Xtreme:
 *   UG_HELLO   version, state (bit 0: gate mode on, bit 1: chat bridged,
 *              bit 2: on the bus as UG_USB_PID with WinUSB descriptors),
 *              own Cybiko ID (4), CyOS's name for itself ("USB Gate")
 *   UG_MSG     a message for the gate: sender ID (4), msg id (2), p0 (4),
 *              p1 (4), port it came to (2), its data
 *   UG_RESULT  how a UG_SEND went: tag (2), CyOS's delivery code (1:
 *              UG_DL_*), 0
 *   UG_LOG     a line of text for the PC's log
 * From the PC:
 *   UG_SEND    deliver a message: tag (2), to (Cybiko ID, 4), msg id (2),
 *              p0 (4), p1 (4), the process name (NUL-ended; "#25" is port
 *              25), its data. To UG_BROADCAST for everyone around.
 *   UG_PING    answer with UG_HELLO
 *   UG_QUIT    stop the application
 *   UG_CHAT    on (1) or off (0): take Chat's lines (port 25, RF channel 4)
 *              and hand them over as UG_MSG, so the PC can pass them on
 */
#pragma once

#define UG_PIPE      40       /* MCP pipe 79 to the Xtreme, 80 from it */
#define UG_VERSION   1
#define UG_PORT      0x93     /* the gate service's port */
#define UG_MAX       2048     /* biggest payload */
#define UG_CHAT_PORT 0x25     /* Chat's port */
#define UG_BROADCAST 0xffffffffUL

#define UG_HELLO     0x01
#define UG_MSG       0x02
#define UG_RESULT    0x03
#define UG_LOG       0x04
#define UG_SEND      0x81
#define UG_PING      0x82
#define UG_QUIT      0x83
#define UG_CHAT      0x84

#define UG_STATE_GATE 0x01
#define UG_STATE_CHAT 0x02
#define UG_STATE_WINUSB 0x04

/* While USB Gate runs, the Xtreme is on the bus as 0B66:UG_USB_PID
 * (CyOS alone is 0B66:0041) and tells Windows to give it WinUSB, with
 * this device interface GUID. */
#define UG_USB_VID   0x0b66
#define UG_USB_PID   0x0042
#define UG_USB_GUID  "{A81E3782-6DF8-4A47-8F65-692FACB7C0DB}"


/* Message_check_delivery */
#define UG_DL_NONE    0
#define UG_DL_INQUEUE 1
#define UG_DL_WAIT    2
#define UG_DL_ABORT   3       /* nobody took it */
#define UG_DL_TIMEOUT 4
#define UG_DL_SUCCESS 5
#define UG_DL_NOMEM   0xfe    /* the application had no memory for it */
