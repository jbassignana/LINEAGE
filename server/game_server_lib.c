/*
 * game server library (hot reloadable).
 *
 * threads:
 *   - net thread (game_server.c, epoll/select): receives bytes, frames
 *     and decrypts packets and runs the handlers under the world lock.
 *   - tick thread: simulates the world every CFG_TICK_MS (was 1000ms).
 *   - job threads: per player work in parallel (visibility / interest
 *     management), see gs_world.c.
 *   - sender threads: write each connection's batched output to its
 *     socket outside of the world lock. a slow client no longer stalls
 *     everybody else.
 *
 * the file is a unity build: the other gs_*.c files are included at the
 * bottom so the build scripts stay the same.
 */
#include "game_server_lib.h"

static void init_threads(struct state *state);

int on_pevent(void **buf, enum pevent event, union ppayload *payload)
{
    struct state *state = *(struct state **) buf;
    switch (event) {
    case pevent_init: {
        return on_init(buf);
    } break;
    case pevent_before_reload: {
        // the code of these threads lives in this library: they must be
        // stopped (not killed) before it gets unloaded.
        stop_threads(state);
    } break;
    case pevent_after_reload: {
        init_threads(state);
    } break;
    case pevent_socket_connection: {
        l2_mutex_lock(&state->lock);
        on_connection(buf, payload->pevent_socket.socket);
        l2_mutex_unlock(&state->lock);
    } break;
    case pevent_socket_request: {
        l2_mutex_lock(&state->lock);
        state->now_ms = l2_now_ms();
        on_request(buf,
                   payload->pevent_socket.socket,
                   payload->pevent_socket.read,
                   payload->pevent_socket.len);
        flush_all(state);
        l2_mutex_unlock(&state->lock);
    } break;
    case pevent_socket_disconnected: {
        l2_mutex_lock(&state->lock);
        on_disconnect(buf, payload->pevent_socket.socket);
        flush_all(state);
        l2_mutex_unlock(&state->lock);
    } break;
    default:
    break;
    }

    return 1;
}

static int on_init(void **buf)
{
    assert(buf);
    size_t to_alloc = sizeof(struct state);
    *buf = calloc(1, to_alloc);
    if (!*buf) {
        trace("unable to allocate memory for game server" nl);
        return 0;
    }
    struct state *state = (struct state *) *buf;
    state->rng = 0x12345678u ^ (u32) time(0);
    state->now_ms = l2_now_ms();
    init_threads(state);
    setlocale(LC_ALL, "");
    return 1;
}

/* ------------------------------------------------------------- tick thread */

L2_THREAD_FN(timer_thread)
{
    struct state *state = (struct state *) arg;
    u64 next = l2_now_ms() + CFG_TICK_MS;
    u64 last_stats = l2_now_ms();

    while (1) {
        u64 now = l2_now_ms();
        if (now < next)
            l2_sleep_ms((u32) (next - now));
        // fixed rate: if a tick ran late, don't try to catch up in a burst.
        next += CFG_TICK_MS;
        now = l2_now_ms();
        if (next < now)
            next = now + CFG_TICK_MS;

        l2_mutex_lock(&state->lock);
        if (state->stop_timer) {
            l2_mutex_unlock(&state->lock);
            break;
        }
        u64 t0 = l2_now_ms();
        on_tick(state);
        flush_all(state);
        double took = (double) (l2_now_ms() - t0);
        state->stat_ticks++;
        state->stat_tick_ms_total += took;
        if (took > state->stat_tick_ms_max)
            state->stat_tick_ms_max = took;

        if (CFG_STATS_MS && now - last_stats >= CFG_STATS_MS && state->stat_ticks) {
            u32 players = 0;
            for (size_t i = 0; i < countof(state->characters); i++)
                players += state->characters[i].active && state->characters[i].conn;
            u64 bytes = 0, packets = 0;
            for (size_t i = 0; i < countof(state->connections); i++) {
                struct connection *c = state->connections + i;
                l2_mutex_lock(&c->out_lock);
                bytes += c->stat_bytes;
                packets += c->stat_packets;
                c->stat_bytes = 0;
                c->stat_packets = 0;
                l2_mutex_unlock(&c->out_lock);
            }
            double secs = (double) (now - last_stats) / 1000.0;
            if (players)
            trace("stats: players=%u ticks=%llu avg_tick=%.2fms max_tick=%.2fms out=%.1fKB/s packets=%.0f/s" nl,
                  players,
                  (unsigned long long) state->stat_ticks,
                  state->stat_ticks ? state->stat_tick_ms_total / (double) state->stat_ticks : 0.0,
                  state->stat_tick_ms_max,
                  (double) bytes / 1024.0 / secs,
                  (double) packets / secs);
            state->stat_ticks = 0;
            state->stat_tick_ms_total = 0;
            state->stat_tick_ms_max = 0;
            last_stats = now;
        }
        l2_mutex_unlock(&state->lock);
    }
    L2_THREAD_RETURN;
}

/* ----------------------------------------------------------- sender threads */

static void sender_push(struct sender *s, u32 conn_index)
{
    l2_mutex_lock(&s->lock);
    if (s->count < countof(s->ready)) {
        s->ready[(s->head + s->count) % countof(s->ready)] = conn_index;
        s->count++;
    }
    l2_cond_signal(&s->wake);
    l2_mutex_unlock(&s->lock);
}

// write as much as possible of the connection's output. returns 1 if the
// socket buffer was full and there is still data left.
static int sender_write(struct state *state, struct connection *conn)
{
    int blocked = 0;
    l2_mutex_lock(&conn->io_lock);
    while (conn->io_socket) {
        if (conn->sent >= conn->inflight_count) {
            conn->sent = 0;
            conn->inflight_count = 0;
            // grab everything the game logic queued since last time.
            l2_mutex_lock(&conn->out_lock);
            if (conn->to_send_count) {
                memcpy(conn->inflight, conn->to_send, conn->to_send_count);
                conn->inflight_count = conn->to_send_count;
                conn->to_send_count = 0;
            }
            conn->queued = 0;
            l2_mutex_unlock(&conn->out_lock);
            if (!conn->inflight_count)
                break;
        }
        unsigned long long left = conn->inflight_count - conn->sent;
        unsigned long long n = net_send(conn->io_socket, conn->inflight + conn->sent, left);
        conn->sent += n;
        if (n < left) {
            blocked = 1;
            break;
        }
    }
    if (blocked) {
        conn->retry_at_ms = l2_now_ms() + 2;
    } else if (!conn->io_socket) {
        conn->inflight_count = 0;
        conn->sent = 0;
    }
    l2_mutex_unlock(&conn->io_lock);
    (void) state;
    return blocked;
}

L2_THREAD_FN(sender_thread)
{
    struct sender *s = (struct sender *) arg;
    struct state *state = s->state;
    u32 not_ready_streak = 0;

    while (1) {
        l2_mutex_lock(&s->lock);
        while (!s->count && !s->stop)
            l2_cond_wait(&s->wake, &s->lock);
        if (s->stop) {
            l2_mutex_unlock(&s->lock);
            break;
        }
        // everything left in the ring is waiting for a full socket buffer
        // to drain: sleep a bit instead of spinning.
        if (not_ready_streak >= s->count) {
            l2_cond_timedwait_ms(&s->wake, &s->lock, 2);
            not_ready_streak = 0;
        }
        u32 index = s->ready[s->head];
        s->head = (s->head + 1) % countof(s->ready);
        s->count--;
        l2_mutex_unlock(&s->lock);

        struct connection *conn = state->connections + index;
        u64 retry_at = 0;
        l2_mutex_lock(&conn->io_lock);
        retry_at = conn->retry_at_ms;
        l2_mutex_unlock(&conn->io_lock);
        if (retry_at && l2_now_ms() < retry_at) {
            not_ready_streak++;
            sender_push(s, index);
            continue;
        }
        not_ready_streak = 0;
        if (sender_write(state, conn))
            sender_push(s, index);
    }
    L2_THREAD_RETURN;
}

/* --------------------------------------------------------------- job pool */

L2_THREAD_FN(job_thread)
{
    struct jobs *jobs = (struct jobs *) arg;
    // worker index is derived from its slot in the threads array.
    l2_mutex_lock(&jobs->lock);
    int me = jobs->pending++;  // pending is used as a start counter here.
    l2_cond_broadcast(&jobs->done);
    u64 seen = jobs->generation;
    l2_mutex_unlock(&jobs->lock);

    while (1) {
        l2_mutex_lock(&jobs->lock);
        while (jobs->generation == seen && !jobs->stop)
            l2_cond_wait(&jobs->start, &jobs->lock);
        if (jobs->stop) {
            l2_mutex_unlock(&jobs->lock);
            break;
        }
        seen = jobs->generation;
        void (*fn)(void *, size_t, size_t) = jobs->fn;
        void *ctx = jobs->ctx;
        size_t n = jobs->n;
        size_t parts = (size_t) jobs->count + 1;
        l2_mutex_unlock(&jobs->lock);

        size_t part = (size_t) me + 1;
        size_t from = n * part / parts;
        size_t to = n * (part + 1) / parts;
        if (from < to)
            fn(ctx, from, to);

        l2_mutex_lock(&jobs->lock);
        jobs->pending--;
        if (!jobs->pending)
            l2_cond_broadcast(&jobs->done);
        l2_mutex_unlock(&jobs->lock);
    }
    L2_THREAD_RETURN;
}

// split [0, n) between the calling thread and the job threads and wait
// until everything is done. fn must only touch data owned by its range.
static void jobs_parallel_for(struct jobs *jobs, size_t n, void (*fn)(void *, size_t, size_t), void *ctx)
{
    // small batches are faster on one thread than waking everybody.
    if (!jobs->count || n < 32) {
        fn(ctx, 0, n);
        return;
    }
    l2_mutex_lock(&jobs->lock);
    jobs->fn = fn;
    jobs->ctx = ctx;
    jobs->n = n;
    jobs->pending = jobs->count;
    jobs->generation++;
    l2_cond_broadcast(&jobs->start);
    l2_mutex_unlock(&jobs->lock);

    size_t parts = (size_t) jobs->count + 1;
    size_t to = n / parts;
    if (to)
        fn(ctx, 0, to);

    l2_mutex_lock(&jobs->lock);
    while (jobs->pending)
        l2_cond_wait(&jobs->done, &jobs->lock);
    l2_mutex_unlock(&jobs->lock);
}

static void init_threads(struct state *state)
{
    assert(state);
    int cpus = l2_cpu_count();

    l2_mutex_init(&state->lock);
    for (size_t i = 0; i < countof(state->connections); i++) {
        l2_mutex_init(&state->connections[i].out_lock);
        l2_mutex_init(&state->connections[i].io_lock);
    }

    // senders.
    int senders = CFG_SENDER_THREADS ? CFG_SENDER_THREADS : cpus - 1;
    if (senders < 1)
        senders = 1;
    if (senders > CFG_MAX_SENDERS)
        senders = CFG_MAX_SENDERS;
    state->sender_count = senders;
    for (int i = 0; i < senders; i++) {
        struct sender *s = state->senders + i;
        s->state = state;
        s->head = 0;
        s->count = 0;
        s->stop = 0;
        l2_mutex_init(&s->lock);
        l2_cond_init(&s->wake);
        l2_thread_start(&s->thread, sender_thread, s);
    }

    // job pool: the calling thread works too, so cpus - 1 helpers.
    struct jobs *jobs = &state->jobs;
    int workers = CFG_JOB_THREADS ? CFG_JOB_THREADS - 1 : cpus - 1;
    if (workers < 0)
        workers = 0;
    if (workers > CFG_MAX_JOB_THREADS)
        workers = CFG_MAX_JOB_THREADS;
    l2_mutex_init(&jobs->lock);
    l2_cond_init(&jobs->start);
    l2_cond_init(&jobs->done);
    jobs->stop = 0;
    jobs->pending = 0;
    jobs->count = 0;
    for (int i = 0; i < workers; i++)
        l2_thread_start(&jobs->threads[i], job_thread, jobs);
    // wait until all workers registered their index.
    l2_mutex_lock(&jobs->lock);
    while (jobs->pending < workers)
        l2_cond_wait(&jobs->done, &jobs->lock);
    jobs->pending = 0;
    jobs->count = workers;
    l2_mutex_unlock(&jobs->lock);

    state->stop_timer = 0;
    l2_thread_start(&state->timer, timer_thread, state);

    trace("game server threads: 1 net + 1 tick + %d sender + %d job workers (%d cpus)" nl,
          senders, workers, cpus);
}

static void stop_threads(struct state *state)
{
    l2_mutex_lock(&state->lock);
    state->stop_timer = 1;
    l2_mutex_unlock(&state->lock);
    l2_thread_join(state->timer);

    l2_mutex_lock(&state->jobs.lock);
    state->jobs.stop = 1;
    l2_cond_broadcast(&state->jobs.start);
    l2_mutex_unlock(&state->jobs.lock);
    for (int i = 0; i < state->jobs.count; i++)
        l2_thread_join(state->jobs.threads[i]);

    for (int i = 0; i < state->sender_count; i++) {
        struct sender *s = state->senders + i;
        l2_mutex_lock(&s->lock);
        s->stop = 1;
        l2_cond_signal(&s->wake);
        l2_mutex_unlock(&s->lock);
        l2_thread_join(s->thread);
        l2_cond_destroy(&s->wake);
        l2_mutex_destroy(&s->lock);
    }
}

/* ------------------------------------------------------------------ output */

static u32 conn_index(struct state *state, struct connection *conn)
{
    return (u32) (conn - state->connections);
}

// hand the connection's queued packets to its sender thread.
static void conn_flush(struct state *state, struct connection *conn)
{
    int schedule = 0;
    l2_mutex_lock(&conn->out_lock);
    if (conn->to_send_count && !conn->queued) {
        conn->queued = 1;
        schedule = 1;
    }
    l2_mutex_unlock(&conn->out_lock);
    if (schedule) {
        u32 index = conn_index(state, conn);
        sender_push(state->senders + (index % (u32) state->sender_count), index);
    }
}

// called once per tick and after each batch of requests, so the packets
// produced in between go out together (fewer syscalls, fewer tcp
// segments, and the client processes them in one go).
static void flush_all(struct state *state)
{
    for (size_t i = 0; i < countof(state->connections); i++) {
        struct connection *conn = state->connections + i;
        if (!conn->socket)
            continue;
        conn_flush(state, conn);
    }
}

static void kick(struct connection *conn)
{
    if (!conn || !conn->socket)
        return;
    // shutdown instead of close: the net thread will see the connection
    // ending, fire on_disconnect and only then release the socket.
    net_shutdown(conn->socket);
}

/* ------------------------------------------------------------------ events */

static void on_tick(struct state *state)
{
    u64 now = l2_now_ms();
    u64 prev = state->now_ms ? state->now_ms : now;
    state->now_ms = now;
    state->d = (float) (now - prev);
    state->run_time += (double) state->d;
    state->ticks = (u64) state->run_time / 100;

    for (size_t i = 0; i < countof(state->characters); i++) {
        struct character *character = state->characters + i;
        if (!character->active || !character->in_world)
            continue;
        character_update(state, character);
    }

    if (now - state->last_visibility_ms >= CFG_VISIBILITY_MS) {
        state->last_visibility_ms = now;
        update_visibility(state);
    }
}

// reset everything but the locks (a sender thread may still be holding
// them for this slot's previous owner, so they can't be re-created).
static void conn_reset(struct connection *conn, int socket)
{
    l2_mutex_lock(&conn->io_lock);
    l2_mutex_lock(&conn->out_lock);
    size_t offset = offsetof(struct connection, socket);
    memset((byte *) conn + offset, 0, sizeof(*conn) - offset);
    conn->socket = socket;
    conn->io_socket = socket;
    l2_mutex_unlock(&conn->out_lock);
    l2_mutex_unlock(&conn->io_lock);
}

static void on_connection(void **buf, int socket)
{
    assert(buf);

    struct state *state = *(struct state **) buf;
    assert(state);
    ptrace("new connection to game server!" nl);
    struct connection *connection = get_connection_from_socket(state, socket);
    if (!connection) {
        trace("there is no more space to accept new players. dropping new connection" nl);
        net_shutdown(socket);
        return;
    }
    conn_reset(connection, socket);

    byte key[] = {0x94, 0x35, 0x00, 0x00, 0xa1, 0x6c, 0x54, 0x87};
    memcpy(connection->encrypt_key, key, sizeof(key));
    memcpy(connection->decrypt_key, key, sizeof(key));
}

static void on_request(void **buf, int socket, void *request, size_t len)
{
    assert(buf);
    struct state *state = *(struct state **) buf;
    assert(state);
    struct connection *conn = get_connection_from_socket(state, socket);
    if (!conn || conn->socket != socket) {
        trace("request for a non connected client?" nl);
        return;
    }
    // the original copied without checking: a client sending more than
    // 8kb without a complete packet overflowed the buffer.
    if (len > sizeof(conn->request) - conn->request_count) {
        warn("client sent too much data without a full packet. dropping it." nl);
        kick(conn);
        return;
    }
    memcpy(conn->request + conn->request_count, request, len);
    conn->request_count += len;
    while (conn->socket && handle_request(state, conn));
}

static void on_disconnect(void **buf, int socket)
{
    assert(buf);
    struct state *state = *(struct state **) buf;
    assert(state);

    struct connection *conn = 0;
    for (size_t i = 0; i < countof(state->connections); i++) {
        if (state->connections[i].socket == socket) {
            conn = state->connections + i;
            break;
        }
    }
    if (!conn)
        return;
    ptrace("client disconnected" nl);
    // after this no sender thread will touch the socket again, so the
    // net layer can close it safely once we return.
    l2_mutex_lock(&conn->io_lock);
    conn->io_socket = 0;
    l2_mutex_unlock(&conn->io_lock);
    conn->socket = 0;
    if (conn->character) {
        character_leave_world(state, conn->character);
        conn->character->conn = 0;
        conn->character = 0;
    }
}

static void character_update(struct state *state, struct character *character)
{
    switch (character->action_type) {
    case idle:
        break;
    case moving:
        moving_update(state, character);
        break;
    case attacking:
        attacking_update(state, character);
        break;
    case casting:
        casting_update(state, character);
        break;
    case dead:
        // npc corpses disappear after a while.
        if (!character->conn && state->now_ms - character->dead_since_ms >= CFG_NPC_CORPSE_MS)
            character_leave_world(state, character);
        break;
    default:
        break;
    }
    if (character->active)
        world_character_tick(state, character);
}

/* ------------------------------------------------------- packet encoding */

static byte *bscanf_va(byte *src, size_t n, va_list va)
{
    byte *tail = src;
    byte *end = src + n;
    while (1) {
        char *fmt = va_arg(va, char *);
        if (fmt == 0)
            break;
        if (fmt[0] == '%' && fmt[1] == 'l' && fmt[2] == 's') {
            size_t max_letters = va_arg(va, size_t);
            wchar_t *dest = va_arg(va, wchar_t *);
            /*
             * wchar_t is 2 bytes in windows and 4 in linux, which means
             * we can't use wcsncpy: lineage sends two bytes per character.
             * bounded by both the destination and the packet.
             */
            size_t i = 0;
            while (tail + 1 < end && (tail[0] | tail[1])) {
                if (i + 1 < max_letters)
                    dest[i++] = (wchar_t) ((tail[1] << 8) | tail[0]);
                tail += 2;
            }
            dest[i] = 0;
            tail += 2;
            continue;
        }
        // read fixed size values only if they are inside the packet,
        // otherwise leave the destination untouched (zeroed by callers).
#define read_fixed(type) \
        { type *dest = va_arg(va, type *); \
          if (tail + sizeof(type) <= end) memcpy(dest, tail, sizeof(type)); \
          tail += sizeof(type); continue; }
        if (fmt[0] == '%' && fmt[1] == 'l' && fmt[2] == 'f')
            read_fixed(double)
        if (fmt[0] == '%' && fmt[1] == 'u')
            read_fixed(u32)
        if (fmt[0] == '%' && fmt[1] == 'h')
            read_fixed(u16)
        if (fmt[0] == '%' && fmt[1] == 'c')
            read_fixed(u8)
#undef read_fixed
        if (fmt[0] == '%' && fmt[1] == 'x') {
            size_t bytes_to_skip = va_arg(va, size_t);
            tail += bytes_to_skip;
            continue;
        }

        error("unexpected format '%s' in bscanf." nl, fmt);
        assert(0);
    }
    return tail;
}

static byte *bscanf_(byte *src, size_t n, ...)
{
    va_list va;
    va_start(va, n);
    byte *result = bscanf_va(src, n, va);
    va_end(va);
    return result;
}

// returns 0 if the packet doesn't fit in n bytes.
static byte *bprintf_va(byte *dest, size_t n, va_list va)
{
    byte *tail = dest;
    byte *end = dest + n;
#define need(bytes) if ((size_t) (end - tail) < (size_t) (bytes)) return 0
    while (1) {
        char *fmt = va_arg(va, char *);
        if (fmt == 0)
            break;
        if (fmt[0] == '%' && fmt[1] == 'l' && fmt[2] == 's') {
            size_t max_letters = va_arg(va, size_t);
            wchar_t *src = va_arg(va, wchar_t *);
            for (size_t i = 0; i < max_letters && *src; i++) {
                need(2);
                u16 letter = (u16) *src++;
                memcpy(tail, &letter, 2);
                tail += 2;
            }
            need(2);
            tail[0] = 0;
            tail[1] = 0;
            tail += 2;
            continue;
        }
        if (fmt[0] == '%' && fmt[1] == 'u') {
            u32 src = va_arg(va, u32);
            need(4);
            memcpy(tail, &src, 4);
            tail += 4;
            continue;
        }
        if (fmt[0] == '%' && fmt[1] == 'h') {
            u16 src = (u16) va_arg(va, u32);
            need(2);
            memcpy(tail, &src, 2);
            tail += 2;
            continue;
        }
        if (fmt[0] == '%' && fmt[1] == 'c') {
            u8 src = (u8) va_arg(va, u32);
            need(1);
            *tail++ = src;
            continue;
        }
        if (fmt[0] == '%' && fmt[1] == 'l' && fmt[2] == 'f') {
            double src = va_arg(va, double);
            need(8);
            memcpy(tail, &src, 8);
            tail += 8;
            continue;
        }
        if (fmt[0] == '%' && fmt[1] == 'x') {
            size_t bytes_to_skip = va_arg(va, size_t);
            need(bytes_to_skip);
            memset(tail, 0, bytes_to_skip);
            tail += bytes_to_skip;
            continue;
        }
        if (fmt[0] == '%' && fmt[1] == 'b') {
            size_t bytes_to_write = va_arg(va, size_t);
            byte *src = va_arg(va, byte *);
            need(bytes_to_write);
            memcpy(tail, src, bytes_to_write);
            tail += bytes_to_write;
            continue;
        }

        error("unexpected format '%s' in bprintf." nl, fmt);
        assert(0);
    }
#undef need
    return tail;
}

static byte *bprintf_(byte *dest, size_t n, ...)
{
    va_list va;
    va_start(va, n);
    byte *result = bprintf_va(dest, n, va);
    va_end(va);
    return result;
}

static u16 packet_size(byte *src)
{
    u16 size = 0;
    memcpy(&size, src, 2);
    return size;
}

static void push_response_va(struct connection *conn, int _encrypt, va_list va)
{
    if (!conn || !conn->socket)
        return;

    byte packet[16 kb];
    byte *end = bprintf_va(packet, sizeof(packet), va);
    if (!end) {
        error("packet too big, not sent." nl);
        return;
    }
    u16 size = (u16) (end - packet);
    memcpy(packet, &size, 2);

    l2_mutex_lock(&conn->out_lock);
    if (conn->overflowed || conn->to_send_count + size > sizeof(conn->to_send)) {
        // the client is not reading fast enough (or is gone). dropping
        // packets would desync its cipher, so drop the client instead.
        if (!conn->overflowed)
            warn("output buffer full for %ls (%zu bytes waiting), disconnecting." nl,
                 conn->username, conn->to_send_count);
        conn->overflowed = 1;
        l2_mutex_unlock(&conn->out_lock);
        kick(conn);
        return;
    }
    // encrypt in queue order: the xor key advances per packet.
    if (_encrypt)
        encrypt(conn, packet);
    memcpy(conn->to_send + conn->to_send_count, packet, size);
    conn->to_send_count += size;
    conn->stat_bytes += size;
    conn->stat_packets++;
    l2_mutex_unlock(&conn->out_lock);
}

static void push_response_(struct connection *conn, int _encrypt, ...)
{
    va_list va;
    va_start(va, _encrypt);
    push_response_va(conn, _encrypt, va);
    va_end(va);
}

// send to `character` itself (if it's a player) and to every player that
// currently knows about it. the original sent everything to everybody in
// the world, so a fight in one town made every client in every other
// town process (and render) it.
static void broadcast_(struct state *state, struct character *character, int _encrypt, ...)
{
    assert(state);
    assert(character);
    u32 id = get_character_id(state, character);
    for (size_t i = 0; i < countof(state->characters); i++) {
        struct character *observer = state->characters + i;
        if (!observer->active || !observer->conn || !observer->in_world)
            continue;
        if (observer != character && !knows(observer, id))
            continue;
        va_list va;
        va_start(va, _encrypt);
        push_response_va(observer->conn, _encrypt, va);
        va_end(va);
    }
}

static void broadcast_others_(struct state *state, struct character *character, int _encrypt, ...)
{
    assert(state);
    assert(character);
    u32 id = get_character_id(state, character);
    for (size_t i = 0; i < countof(state->characters); i++) {
        struct character *observer = state->characters + i;
        if (observer == character || !observer->active || !observer->conn || !observer->in_world)
            continue;
        if (!knows(observer, id))
            continue;
        va_list va;
        va_start(va, _encrypt);
        push_response_va(observer->conn, _encrypt, va);
        va_end(va);
    }
}

static void encrypt(struct connection *conn, byte *packet)
{
    assert(conn);
    assert(packet);

    u16 size = packet_size(packet);
    if (size > 1)
        size -= 2;

    byte *body = packet + 2;

    u32 temp = 0;
    u32 temp2 = 0;
    for (u16 i = 0; i < size; i++) {
        temp2 = body[i] & 0xff;
        body[i] = (byte) (temp2 ^ conn->encrypt_key[i & 7] ^ temp);
        temp = body[i];
    }

    u32 old = ((u32) conn->encrypt_key[0] & 0xff);
    old |= ((u32) conn->encrypt_key[1] << 8 & 0xff00);
    old |= ((u32) conn->encrypt_key[2] << 0x10 & 0xff0000);
    old |= ((u32) conn->encrypt_key[3] << 0x18 & 0xff000000);

    old += size;

    conn->encrypt_key[0] = (byte) (old & 0xff);
    conn->encrypt_key[1] = (byte) (old >> 0x08 & 0xff);
    conn->encrypt_key[2] = (byte) (old >> 0x10 & 0xff);
    conn->encrypt_key[3] = (byte) (old >> 0x18 & 0xff);
}

static void decrypt(struct connection *conn, byte *request)
{
    assert(conn);
    assert(request);

    u16 size = packet_size(request);
    if (size > 1)
        size -= 2;

    byte *body = request + 2;
    u32 temp = 0;
    u32 temp2 = 0;
    for (u16 i = 0; i < size; i++) {
        temp2 = body[i];
        body[i] = (byte) (temp2 ^ conn->decrypt_key[i & 7] ^ temp);
        temp = temp2;
    }

    u32 old = conn->decrypt_key[0] & 0xff;
    old |= conn->decrypt_key[1] << 8 & 0xff00;
    old |= conn->decrypt_key[2] << 0x10 & 0xff0000;
    old |= (u32) conn->decrypt_key[3] << 0x18 & 0xff000000;

    old += size;

    conn->decrypt_key[0] = (byte) (old & 0xff);
    conn->decrypt_key[1] = (byte) (old >> 0x08 & 0xff);
    conn->decrypt_key[2] = (byte) (old >> 0x10 & 0xff);
    conn->decrypt_key[3] = (byte) (old >> 0x18 & 0xff);
}

static struct connection *get_connection_from_socket(struct state *state, int socket)
{
    assert(state);
    for (size_t i = 0; i < countof(state->connections); i++) {
        if (state->connections[i].socket == socket)
            return state->connections + i;
    }
    // find a free connection to be used for this socket.
    for (size_t i = 0; i < countof(state->connections); i++) {
        if (!state->connections[i].socket)
            return state->connections + i;
    }
    return 0;
}

static void handle_request_by_type(struct state *state, struct connection *conn)
{
    byte *request = conn->request;

    byte type = *(request + 2);
    ptrace("received packet is of type %#04x" nl, (int) type);

    // check if the received packet matches the expected packet.
    // if it doesn't, drop/close the connection.
#define expected_or_close(expected) \
if (type != (expected)) { \
error("expecting packet %d but got %d. the connection will be dropped." nl, \
(expected), \
type); \
kick(conn); \
return; \
}

    // packets that only make sense once the character is in the world.
    // the original dereferenced conn->character without checking, so a
    // client sending them early crashed the whole server.
#define in_world_or_ignore() \
if (!conn->character || !conn->character->active || !conn->character->in_world) \
break;

    coroutine(conn->state) {
        // expect protocol request.
        expected_or_close(0x00);
        handle_send_protocol(state, conn);
        conn->encrypted = 1;

        yield;
        // expect auth request.
        expected_or_close(0x08);
        handle_auth(state, conn, request);

        yield;
        switch (type) {
            // show create character screen.
            case 0x0e:
            handle_show_create_character_screen(state, conn);
            break;
            // create character.
            case 0x0b:
            handle_character_creation(state, conn, request);
            break;
            // select character
            case 0x0d:
            handle_select_character(state, conn, request);
            break;
            // bsps
            case 0xd0:
            handle_auto_ss_bsps(state, conn);
            break;
            // quest list
            case 0x63:
            handle_send_quest_list(state, conn);
            break;
            // enter world
            case 0x03:
            if (!conn->character)
                break;
            handle_enter_world(state, conn);
            break;
            // quit
            case 0x09:
            handle_leave_world(state, conn);
            break;
            // restart
            case 0x46:
            in_world_or_ignore();
            handle_restart(state, conn);
            break;
            // move (mouse click or keyboard)
            case 0x01:
            in_world_or_ignore();
            handle_movement(state, conn, request);
            break;
            // validate position
            case 0x48:
            in_world_or_ignore();
            handle_validate_position(state, conn, request);
            break;
            // cannot move anymore (client stopped, e.g. key released)
            case 0x36:
            in_world_or_ignore();
            handle_cannot_move_anymore(state, conn, request);
            break;
            // show map
            case 0xcd:
            in_world_or_ignore();
            handle_show_map(state, conn);
            break;
            // action
            case 0x04:
            in_world_or_ignore();
            handle_action(state, conn, request);
            break;
            // attack request (ctrl+click / attack button)
            case 0x0a:
            in_world_or_ignore();
            handle_attack_request(state, conn, request);
            break;
            // use skill
            case 0x2f:
            in_world_or_ignore();
            handle_magic_skill_use(state, conn, request);
            break;
            // skill list
            case 0x3f:
            in_world_or_ignore();
            send_skill_list(state, conn->character);
            break;
            // deselect target
            case 0x37:
            in_world_or_ignore();
            handle_deselect_target(state, conn);
            break;
            default:
            break;
        }
    }
#undef in_world_or_ignore
#undef expected_or_close
}

static int handle_request(struct state *state, struct connection *conn)
{
    assert(state);
    assert(conn);
    byte *request = conn->request;

    if (conn->request_count < 2)
        return 0;
    u16 size = packet_size(request);
    if (size < 3 || size > sizeof(conn->request)) {
        // size 0 made the original loop forever.
        warn("invalid packet size %u. dropping client." nl, (u32) size);
        conn->request_count = 0;
        kick(conn);
        return 0;
    }
    if (size > conn->request_count)
        return 0;

    if (conn->encrypted)
        decrypt(conn, request);

    handle_request_by_type(state, conn);

    memmove(conn->request, conn->request + size, conn->request_count - size);
    conn->request_count -= size;

    return 1;
}

static struct character *get_character_by_id(struct state *state, u32 id)
{
    assert(state);
    if (!id || id >= countof(state->characters))
        return 0;
    return state->characters + id;
}

static u32 get_character_id(struct state *state, struct character *src)
{
    assert(state);
    assert(src);
    return (u32) ((size_t) (src - state->characters));
}

static u64 distance_between(s32 x, s32 y, s32 z, s32 x2, s32 y2, s32 z2)
{
    // in double: garbage coordinates from a client can't overflow it.
    double dx = (double) x2 - (double) x;
    double dy = (double) y2 - (double) y;
    double dz = (double) z2 - (double) z;
    double d2 = dx * dx + dy * dy + dz * dz;
    return d2 >= 1.8e19 ? (u64) 1.8e19 : (u64) d2;
}

static u64 distance_between_characters(struct character *a, struct character *b)
{
    if (!a || !a->active || !b || !b->active)
        return 0;
    return distance_between(a->x, a->y, a->z, b->x, b->y, b->z);
}

static u32 rnd(struct state *state)
{
    // xorshift32, only touched under the world lock.
    u32 x = state->rng ? state->rng : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state->rng = x;
    return x;
}

static float rndf(struct state *state)
{
    return (float) (rnd(state) & 0xffffff) / (float) 0x1000000;
}

#include "gs_login.c"
#include "gs_world.c"
#include "gs_move.c"
#include "gs_combat.c"
