// Self-test / demo sequencer: runs the device's activities back-to-back (music,
// video, Flappy Bird, DOOM) with health logging between each — handy for demos
// and for capturing a full regression snapshot over serial in one shot.
//
// Triggered by the self.test.run MCP tool ("play test" / "run the demo").
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register the self.test.* MCP tools (run / stop).
void demo_sequencer_register_mcp(void);

#ifdef __cplusplus
}
#endif
