/* The kernel exports the USB Gate uses on top of the Browser's (CyOS
 * 1.5.08; numbers found from how CyOS's own console and loader call them,
 * see cybiko-docs usb.md). */
INCLUDE cybiko-browser/cyapp/cyos.ld
_cy_mcp_pipe_factory = 0xf00000 + 1784;  /* data: the MCP pipe factory */
_cy_PipeFactory_open = 0xf00000 + 1680;  /* (factory, name, logical pipe, 0) */
_cy_PipeFactory_release = 0xf00000 + 1672; /* (factory, pipe, 0) */
_cy_MCPInput_available = 0xf00000 + 1604;
_cy_MCPInput_read = 0xf00000 + 1607;     /* (stream, buffer, length) */
_cy_MCPOutput_flush = 0xf00000 + 1622;
_cy_MCPOutput_write = 0xf00000 + 1628;   /* (stream, buffer, length) */
_cy_set_gate_mode = 0xf00000 + 1152;
_cy_get_gate_mode = 0xf00000 + 718;
_cy_Queue_peek_message = 0xf00000 + 940;
_cy_radio_power = 0xf00000 + 1109;      /* (on): 49fc70, wakes the radio up */
_cy_radio = 0xf00000 + 888;             /* data: the radio's object */
_cy_radio_ping = 0xf00000 + 1079;       /* (radio, RF channel): 49db50, sends a ping */
_cy_rf = 0xf00000 + 1044;               /* data: the radio's channel object */
_cy_rf_set_channel = 0xf00000 + 1161;   /* (rf, RF channel 2..9): 49ca04 */
_cy_rf_channel = 0xf00000 + 1072;       /* (rf): 49ca66 */
_cy_clock = 0xf00000 + 450;             /* CyOS's clock in 0.1 ms */
