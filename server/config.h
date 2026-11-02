#ifndef L2_CONFIG_H
#define L2_CONFIG_H
/*
 * game server tunables. everything that changes how much the client
 * has to process (and therefore its fps in crowded places) lives here.
 */

/* ---------------------------------------------------------------- threads */
// world simulation rate. the original server ticked once per second,
// which made movement, attacks and casts land in 1 second bursts.
#define CFG_TICK_MS 100
// threads that flush per-connection output buffers to sockets.
// 0 = automatic (cpu count - 1, clamped to [1, CFG_MAX_SENDERS]).
#define CFG_SENDER_THREADS 0
#define CFG_MAX_SENDERS 8
// worker threads used for parallel per-player work (visibility).
// 0 = automatic (cpu count).
#define CFG_JOB_THREADS 0
#define CFG_MAX_JOB_THREADS 16
// per connection outgoing buffer. a client that can't keep up and
// overflows it is disconnected (instead of corrupting memory).
#define CFG_OUTBOX_BYTES (48 * 1024)

/* ------------------------------------------- interest management (fps!) */
// an object enters a player's view when closer than ENTER and leaves
// when farther than LEAVE (hysteresis avoids spawn/despawn flicker).
#define CFG_VIEW_ENTER 3000
#define CFG_VIEW_LEAVE 3600
// hard cap of objects a client is told about. in a 500 player siege the
// c4 client only gets the nearest N, which is what keeps its fps sane.
// your current target and whoever is attacking you are always kept.
#define CFG_MAX_KNOWN 120
// how often visibility is recomputed.
#define CFG_VISIBILITY_MS 200

/* --------------------------------------------------------------- movement */
// max distance between where the client says it is and where the
// server thinks it is, before the server corrects the client.
#define CFG_DESYNC_TOLERANCE 160
// never send position corrections to the same client more often.
#define CFG_CORRECTION_COOLDOWN_MS 1000
// keyboard (arrow keys / wasd) movement. the client sends a far point
// in the direction it walks; clamp it so nobody walks across the map.
#define CFG_KEYBOARD_MAX_LEAD 1200
// other players are told about direction changes at most every N ms,
// unless the direction changed more than ANGLE degrees.
#define CFG_KEYBOARD_BCAST_MS 150
#define CFG_KEYBOARD_BCAST_ANGLE 12
// following a target (attack/cast approach) re-paths at most every N ms
// or when the target moved more than DIST.
#define CFG_FOLLOW_REPATH_MS 400
#define CFG_FOLLOW_REPATH_DIST 40

/* ------------------------------------------------------------------ combat */
#define CFG_ORC_HP 1500
#define CFG_NPC_CORPSE_MS 5000
// after shadow step the target can't turn around nor attack for this long.
#define CFG_SHADOW_STEP_DISORIENT_MS 1200

/* ------------------------------------------------------- protocol (c4) */
// the interlude SkillList appends a 'disabled' byte per skill. c4 doesn't.
#define CFG_SKILLLIST_DISABLED_BYTE 0
// players never saw each other in the original server (CharInfo was a
// todo). the CharInfo layout below follows l2j c4-era code but has NOT
// been validated against a real c4 client yet: enable it and test.
#define CFG_SEND_PLAYER_CHARINFO 0

/* ------------------------------------------------------------ logging */
// the original logged every packet received and every send() to stderr,
// which at a few hundred players costs more cpu than the game itself.
#define CFG_TRACE_PACKETS 0
// print tick time / bandwidth stats every N ms (0 = off).
#define CFG_STATS_MS 10000

#endif
