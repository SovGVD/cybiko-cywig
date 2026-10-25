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
