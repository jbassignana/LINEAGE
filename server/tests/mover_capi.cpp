// tiny C wrapper so tests/l2bot.py can drive the real KeyboardMover
// against the real server (integration test).
#include "../../client/ue5/L2KeyboardMover.h"
extern "C" {
void *km_new(double speed, double x, double y, double z) {
    auto *m = new l2::KeyboardMover();
    m->set_speed(speed);
    m->set_position({x, y, z});
    return m;
}
void km_input(void *m, double f, double r, double yaw) { static_cast<l2::KeyboardMover *>(m)->set_input(f, r, yaw); }
// out: up to 2 packets x 8 ints (kind, x, y, z, tx, ty, tz, heading)
int km_tick(void *m, double now_ms, double dt, int *out) {
    l2::Packet p[2];
    int n = static_cast<l2::KeyboardMover *>(m)->tick(now_ms, dt, p);
    for (int i = 0; i < n; i++) {
        int *o = out + i * 8;
        o[0] = (int) p[i].kind; o[1] = p[i].x; o[2] = p[i].y; o[3] = p[i].z;
        o[4] = p[i].tx; o[5] = p[i].ty; o[6] = p[i].tz; o[7] = p[i].heading;
    }
    return n;
}
void km_server(void *m, double x, double y, double z) { static_cast<l2::KeyboardMover *>(m)->on_server_position({x, y, z}); }
}
