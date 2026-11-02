// L2KeyboardMover: WASD movement for a custom (UE5) lineage 2 client.
//
// pure c++17, no unreal types, so it can be unit tested anywhere and
// dropped into the UE5 plugin (see L2KeyboardMoverComponent.example.h).
//
// what it does better than click-to-move / the official keyboard mode:
//  - client side prediction: the character moves the frame you press W,
//    it doesn't wait for the server echo (no input lag).
//  - camera relative 8-way movement (W/A/S/D + diagonals, normalized).
//  - sends the minimum: a move packet (0x01, movement=0) only when the
//    direction changes more than `turn_deg` or every `resend_ms` to push
//    the lead point forward; a ValidatePosition (0x48) every
//    `validate_ms`; and CannotMoveAnymore (0x36) on release, so other
//    players see you stop exactly where you stopped.
//  - server corrections are blended over `blend_ms` instead of snapping
//    (small ones ignored, huge ones snapped).
//
// it pairs with the l2auth server changes: the server trusts positions
// within CFG_DESYNC_TOLERANCE, throttles what observers get and detects
// key release from ValidatePosition.
#pragma once
#include <cmath>
#include <cstdint>

namespace l2 {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

enum class PacketKind { Move, Validate, Stop };

// what to send. map to packets:
//   Move     -> 0x01 MoveBackwardToLocation: target xyz, origin xyz, movement=0
//   Validate -> 0x48 ValidatePosition: x, y, z, heading, 0
//   Stop     -> 0x36 CannotMoveAnymore: x, y, z, heading
struct Packet {
    PacketKind kind;
    int32_t x, y, z;          // origin / current position
    int32_t tx, ty, tz;       // move target (Move only)
    int32_t heading;          // 0..65535
};

struct KeyboardMoverConfig {
    double lead = 600;          // how far ahead the move target is placed.
    double resend_ms = 500;     // refresh the lead while holding a key (lead lasts ~5s).
    double turn_deg = 8;        // direction change that forces a send.
    double validate_ms = 400;   // ValidatePosition period while moving.
    double ignore_error = 24;   // server corrections smaller than this are ignored.
    double snap_error = 300;    // bigger than this: teleport, don't blend.
    double blend_ms = 150;      // time to absorb a correction.
    double dead_zone = 0.15;    // analog stick dead zone.
};

class KeyboardMover {
public:
    explicit KeyboardMover(KeyboardMoverConfig cfg = {}) : cfg_(cfg) {}

    void set_speed(double run_speed) { speed_ = run_speed; }
    void set_position(Vec3 p) { pos_ = p; blend_left_ms_ = 0; }
    Vec3 position() const { return pos_; }
    bool moving() const { return moving_; }
    int32_t heading() const { return heading_; }

    // forward/right in [-1, 1] (W=+1 forward, S=-1, D=+1 right, A=-1),
    // camera yaw in radians, l2 world axes (x east, y south like the
    // server: heading 0 = +x).
    void set_input(double forward, double right, double camera_yaw) {
        double fx = std::cos(camera_yaw), fy = std::sin(camera_yaw);
        // right vector: rotate forward by +90 degrees.
        double rx = -fy, ry = fx;
        double dx = forward * fx + right * rx;
        double dy = forward * fy + right * ry;
        double len = std::sqrt(dx * dx + dy * dy);
        if (len < cfg_.dead_zone) {
            want_x_ = want_y_ = 0;
            return;
        }
        // diagonals are not faster (l2 only has run/walk speeds).
        want_x_ = dx / len;
        want_y_ = dy / len;
    }

    // advance one frame. writes up to 2 packets to `out`, returns how many.
    int tick(double now_ms, double dt_s, Packet out[2]) {
        int n = 0;
        bool want = want_x_ != 0 || want_y_ != 0;

        // absorb pending correction.
        if (blend_left_ms_ > 0) {
            double step = std::fmin(1.0, (dt_s * 1000.0) / blend_left_ms_);
            pos_.x += blend_x_ * step;
            pos_.y += blend_y_ * step;
            pos_.z += blend_z_ * step;
            blend_x_ -= blend_x_ * step;
            blend_y_ -= blend_y_ * step;
            blend_z_ -= blend_z_ * step;
            blend_left_ms_ -= dt_s * 1000.0;
        }

        if (want) {
            // predict locally right away.
            pos_.x += want_x_ * speed_ * dt_s;
            pos_.y += want_y_ * speed_ * dt_s;
            heading_ = to_heading(want_x_, want_y_);

            double dot = want_x_ * sent_x_ + want_y_ * sent_y_;
            bool turned = dot < std::cos(cfg_.turn_deg * 3.14159265358979 / 180.0);
            bool stale = now_ms - last_move_ms_ >= cfg_.resend_ms;
            if (!moving_ || turned || stale) {
                out[n++] = make(PacketKind::Move);
                sent_x_ = want_x_;
                sent_y_ = want_y_;
                last_move_ms_ = now_ms;
                last_validate_ms_ = now_ms;
            } else if (now_ms - last_validate_ms_ >= cfg_.validate_ms) {
                out[n++] = make(PacketKind::Validate);
                last_validate_ms_ = now_ms;
            }
            moving_ = true;
        } else if (moving_) {
            // released: stop exactly here for everybody.
            out[n++] = make(PacketKind::Stop);
            out[n++] = make(PacketKind::Validate);
            moving_ = false;
            sent_x_ = sent_y_ = 0;
        }
        return n;
    }

    // ValidateLocation / position correction for our own character.
    void on_server_position(Vec3 server) {
        double ex = server.x - pos_.x, ey = server.y - pos_.y, ez = server.z - pos_.z;
        double err = std::sqrt(ex * ex + ey * ey);
        if (err < cfg_.ignore_error) {
            pos_.z = server.z;  // no geodata on our side: trust the server's z.
            return;
        }
        if (err > cfg_.snap_error) {
            set_position(server);
            return;
        }
        blend_x_ = ex;
        blend_y_ = ey;
        blend_z_ = ez;
        blend_left_ms_ = cfg_.blend_ms;
    }

private:
    static int32_t to_heading(double dx, double dy) {
        double a = std::atan2(dy, dx);
        if (a < 0) a += 2.0 * 3.14159265358979;
        return static_cast<int32_t>(a * 65536.0 / (2.0 * 3.14159265358979)) & 0xffff;
    }

    Packet make(PacketKind kind) const {
        Packet p{};
        p.kind = kind;
        p.x = static_cast<int32_t>(std::lround(pos_.x));
        p.y = static_cast<int32_t>(std::lround(pos_.y));
        p.z = static_cast<int32_t>(std::lround(pos_.z));
        p.tx = static_cast<int32_t>(std::lround(pos_.x + want_x_ * cfg_.lead));
        p.ty = static_cast<int32_t>(std::lround(pos_.y + want_y_ * cfg_.lead));
        p.tz = p.z;
        p.heading = heading_;
        return p;
    }

    KeyboardMoverConfig cfg_;
    Vec3 pos_;
    double speed_ = 120;
    double want_x_ = 0, want_y_ = 0;
    double sent_x_ = 0, sent_y_ = 0;
    double last_move_ms_ = -1e9, last_validate_ms_ = -1e9;
    bool moving_ = false;
    int32_t heading_ = 0;
    double blend_x_ = 0, blend_y_ = 0, blend_z_ = 0, blend_left_ms_ = 0;
};

}  // namespace l2
