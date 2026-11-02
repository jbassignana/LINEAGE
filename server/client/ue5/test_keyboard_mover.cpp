// g++ -std=c++17 -O2 -Wall -Wextra test_keyboard_mover.cpp -o test_keyboard_mover && ./test_keyboard_mover
#include "L2KeyboardMover.h"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { std::printf("  PASS "); } else { std::printf("  FAIL "); failures++; } std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

int main() {
    using namespace l2;
    const double dt = 1.0 / 60.0;

    // 1. pressing W moves the same frame (prediction) and sends one Move.
    {
        KeyboardMover m;
        m.set_speed(120);
        m.set_position({0, 0, 0});
        m.set_input(1, 0, 0);
        Packet out[2];
        int n = m.tick(0, dt, out);
        CHECK(n == 1 && out[0].kind == PacketKind::Move, "first frame sends a Move");
        CHECK(m.position().x > 1.9, "character moved locally on the first frame (%.2f)", m.position().x);
        CHECK(out[0].tx - out[0].x == 600, "lead point 600 ahead");
    }
    // 2. holding W 3s while steering the camera slowly: few packets.
    {
        KeyboardMover m;
        m.set_speed(120);
        int moves = 0, validates = 0;
        double yaw = 0;
        for (int f = 0; f < 180; f++) {
            yaw += 0.003;  // ~10 degrees per second
            m.set_input(1, 0, yaw);
            Packet out[2];
            int n = m.tick(f * 1000.0 * dt, dt, out);
            for (int i = 0; i < n; i++) {
                moves += out[i].kind == PacketKind::Move;
                validates += out[i].kind == PacketKind::Validate;
            }
        }
        CHECK(moves <= 14, "3s of steering: %d moves (<= ~4/s)", moves);
        CHECK(validates <= 8, "and %d validates", validates);
        CHECK(std::fabs(std::hypot(m.position().x, m.position().y) - 360) < 15, "walked ~360 units in 3s at speed 120 (%.1f)", std::hypot(m.position().x, m.position().y));
    }
    // 3. sharp turn (W -> D) sends immediately.
    {
        KeyboardMover m;
        Packet out[2];
        m.set_input(1, 0, 0);
        m.tick(0, dt, out);
        m.set_input(0, 1, 0);
        int n = m.tick(16, dt, out);
        CHECK(n == 1 && out[0].kind == PacketKind::Move, "90 degree turn sends a Move right away");
        CHECK(out[0].ty > out[0].y && out[0].tx == out[0].x, "D = strafe right (+y in l2 axes)");
    }
    // 4. diagonal is not faster.
    {
        KeyboardMover a, b;
        Packet out[2];
        a.set_input(1, 0, 0);
        b.set_input(1, 1, 0);
        for (int f = 0; f < 60; f++) { a.tick(f * 16.6, dt, out); b.tick(f * 16.6, dt, out); }
        double da = std::hypot(a.position().x, a.position().y), db = std::hypot(b.position().x, b.position().y);
        CHECK(std::fabs(da - db) < 0.5, "W+D covers the same distance as W (%.1f vs %.1f)", db, da);
    }
    // 5. release: Stop + Validate exactly where we are.
    {
        KeyboardMover m;
        Packet out[2];
        m.set_input(1, 0, 0);
        for (int f = 0; f < 30; f++) m.tick(f * 16.6, dt, out);
        m.set_input(0, 0, 0);
        int n = m.tick(500, dt, out);
        CHECK(n == 2 && out[0].kind == PacketKind::Stop && out[1].kind == PacketKind::Validate, "release sends Stop + Validate");
        CHECK(out[0].x == (int) std::lround(m.position().x), "stop at the predicted position");
        CHECK(m.tick(520, dt, out) == 0, "nothing more while idle");
    }
    // 6. corrections: small ignored, medium blended, big snapped.
    {
        KeyboardMover m;
        Packet out[2];
        m.set_position({100, 100, 0});
        m.on_server_position({110, 100, 5});
        CHECK(m.position().x == 100 && m.position().z == 5, "10 units: ignored (z taken from server)");
        m.on_server_position({200, 100, 5});
        CHECK(m.position().x == 100, "100 units: not snapped");
        for (int f = 0; f < 12; f++) m.tick(f * 16.6, dt, out);
        CHECK(std::fabs(m.position().x - 200) < 1.0, "100 units: blended in ~150ms (%.2f)", m.position().x);
        m.on_server_position({5000, 100, 5});
        CHECK(m.position().x == 5000, "big error: snapped");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
