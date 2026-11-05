// PicoCalc keyboard / power controller on I2C1 (STM32 at 0x1F).
// The controller is slow, so register accesses are split into a write
// phase and a delayed read phase to avoid blocking the main loop.
#include "hw/keyboard.h"
#include "log.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#define KBD_I2C i2c1
#define KBD_SDA 6
#define KBD_SCL 7
#define KBD_ADDR 0x1F
#define KBD_BAUD 10000

#define REG_BKL 0x05 // LCD backlight
#define REG_FIF 0x09 // key FIFO
#define REG_BK2 0x0A // keyboard backlight
#define REG_BAT 0x0B // battery
#define REG_WRITE 0x80

#define KEY_STATE_PRESSED 1
#define KEY_STATE_HOLD 2
#define KEY_STATE_RELEASED 3

#define QUEUE_LEN 32
#define READ_DELAY_US 8000
#define POLL_INTERVAL_US 8000
#define BAT_READ_DELAY_US 60000
#define BAT_MAX_RETRIES 12
#define BATTERY_INTERVAL_US 15000000
#define REPEAT_DELAY_US 60000

static key_event_t queue[QUEUE_LEN];
static uint8_t q_head, q_tail;

static uint8_t mods;
static int battery = -1;
static bool has_kbd_backlight;
static int bat_retries;
static bool charging;

static enum { PH_IDLE, PH_WAIT } phase = PH_IDLE;
static uint8_t pending_reg;
static absolute_time_t next_action;
static absolute_time_t next_battery;

static int pending_bkl = -1, pending_bk2 = -1;

// Auto-repeat for held keys (controller only reports a single HOLD event)
static uint8_t held_key;
static absolute_time_t next_repeat;

static uint32_t stat_ok, stat_fail, stat_events;

void kbd_debug_stats(uint32_t *ok, uint32_t *fail, uint32_t *events) {
    *ok = stat_ok;
    *fail = stat_fail;
    *events = stat_events;
}

static void push(uint8_t key) {
    uint8_t n = (q_head + 1) % QUEUE_LEN;
    if (n == q_tail) return;
    queue[q_head].key = key;
    queue[q_head].mods = mods;
    q_head = n;
}

void kbd_inject(uint8_t key) {
    uint8_t n = (q_head + 1) % QUEUE_LEN;
    if (n == q_tail) return;
    queue[q_head].key = key;
    queue[q_head].mods = 0;
    q_head = n;
}

bool kbd_get(key_event_t *ev) {
    if (q_head == q_tail) return false;
    *ev = queue[q_tail];
    q_tail = (q_tail + 1) % QUEUE_LEN;
    return true;
}

static void i2c_setup(void) {
    i2c_init(KBD_I2C, KBD_BAUD);
    gpio_set_function(KBD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SDA, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SCL);
    gpio_pull_up(KBD_SDA);
    phase = PH_IDLE;
}

// If the controller was left mid-transfer and is holding SDA low, clock SCL
// until it lets go. Nothing is sent when the bus is idle: the keyboard MCU
// does not recover from stray START/STOP conditions.
static void bus_recover(void) {
    if (gpio_get(KBD_SDA)) return;
    gpio_set_function(KBD_SCL, GPIO_FUNC_SIO);
    gpio_set_function(KBD_SDA, GPIO_FUNC_SIO);
    gpio_set_dir(KBD_SDA, GPIO_IN);
    gpio_put(KBD_SCL, 0);
    for (int i = 0; i < 18 && !gpio_get(KBD_SDA); i++) {
        gpio_set_dir(KBD_SCL, GPIO_OUT); // drive low
        sleep_us(50);
        gpio_set_dir(KBD_SCL, GPIO_IN);  // release (pulled up)
        sleep_us(50);
    }
    i2c_setup();
}

void kbd_debug_raw_write(uint8_t reg, uint8_t val) {
    uint8_t wb[2] = {reg | REG_WRITE, val}, rsp[2] = {0xEE, 0xEE};
    int w = i2c_write_timeout_us(KBD_I2C, KBD_ADDR, wb, 2, false, 20000);
    sleep_ms(10);
    int r = i2c_read_timeout_us(KBD_I2C, KBD_ADDR, rsp, 2, false, 20000);
    printf("write reg %02x=%02x w->%d r->%d raw=%02x %02x\n", reg, val, w, r, rsp[0], rsp[1]);
    phase = PH_IDLE;
}

void kbd_debug_scan(void) {
    uint8_t b = REG_FIF;
    int r = i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &b, 1, false, 20000);
    printf("KBD probe: SCL=%d SDA=%d write->%d\n", gpio_get(KBD_SCL), gpio_get(KBD_SDA), r);
    if (r == 1) {
        sleep_us(READ_DELAY_US);
        uint8_t rsp[2];
        printf("KBD read->%d\n", i2c_read_timeout_us(KBD_I2C, KBD_ADDR, rsp, 2, false, 20000));
        sleep_ms(20);
        b = REG_BAT;
        int w = i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &b, 1, false, 20000);
        sleep_us(BAT_READ_DELAY_US);
        int rr = -1, tries = 0;
        while (tries++ < BAT_MAX_RETRIES && (rr = i2c_read_timeout_us(KBD_I2C, KBD_ADDR, rsp, 2, false, 20000)) != 2)
            sleep_ms(20);
        printf("keyboard backlight register: %s\n", has_kbd_backlight ? "yes" : "no");
        printf("BAT write->%d read->%d after %d tries raw=%02x %02x cached=%d\n", w, rr, tries, rsp[0], rsp[1],
               battery);
    }
    phase = PH_IDLE;
}

// Factory keyboard firmware has no keyboard-backlight register; newer firmware echoes the
// register number in the first reply byte.
static void probe_features(void) {
    uint8_t b = REG_BK2, rsp[2] = {0, 0};
    if (i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &b, 1, false, 20000) != 1) return;
    sleep_us(READ_DELAY_US);
    if (i2c_read_timeout_us(KBD_I2C, KBD_ADDR, rsp, 2, false, 20000) != 2) return;
    has_kbd_backlight = rsp[0] == REG_BK2;
}

void kbd_init(void) {
    i2c_setup();
    bus_recover();
    probe_features();
    next_action = get_absolute_time();
    next_battery = get_absolute_time();
}

static bool repeatable(uint8_t k) {
    return k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT || k == KEY_BACKSPACE ||
           k == KEY_DEL || k == KEY_PAGE_UP || k == KEY_PAGE_DOWN;
}

static void handle_key(uint8_t state, uint8_t key) {
    stat_events++;
    uint8_t flag = 0;
    if (key == KEY_MOD_CTRL) flag = MOD_CTRL;
    else if (key == KEY_MOD_ALT) flag = MOD_ALT;
    else if (key == KEY_MOD_SHL || key == KEY_MOD_SHR) flag = MOD_SHIFT;
    if (flag) {
        if (state == KEY_STATE_PRESSED || state == KEY_STATE_HOLD) mods |= flag;
        else if (state == KEY_STATE_RELEASED) mods &= ~flag;
        return;
    }
    if (key == KEY_MOD_SYM || key == KEY_CAPS_LOCK) return;

    switch (state) {
    case KEY_STATE_PRESSED:
        push(key);
        break;
    case KEY_STATE_HOLD:
        if (repeatable(key)) {
            held_key = key;
            next_repeat = get_absolute_time();
        }
        break;
    case KEY_STATE_RELEASED:
        if (held_key == key) held_key = 0;
        break;
    }
}

// Consecutive I2C failures; the keyboard MCU is unpowered while the PicoCalc
// is switched off, which is how we detect a power cycle under USB power.
static int fail_count;
static bool came_back;

static void i2c_result(bool ok) {
    if (ok) stat_ok++;
    else stat_fail++;
    if (ok) {
        if (fail_count >= 3) came_back = true;
        if (fail_count >= 10) app_log("Keyboard controller responding again");
        fail_count = 0;
    } else if (fail_count < 1000) {
        fail_count++;
        if (fail_count == 10) app_log("Keyboard controller not responding (try power-cycling the PicoCalc)");
        if (fail_count % 10 == 0) {
            if (fail_count <= 30)
                printf("[kbd] %d consecutive I2C failures, SCL=%d SDA=%d\n", fail_count, gpio_get(KBD_SCL),
                       gpio_get(KBD_SDA));
            bus_recover();
        }
    }
}

bool kbd_take_power_cycle(void) {
    bool r = came_back;
    came_back = false;
    return r;
}

static bool reg_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg | REG_WRITE, val};
    return i2c_write_timeout_us(KBD_I2C, KBD_ADDR, buf, 2, false, 20000) == 2;
}

void kbd_poll(void) {
    absolute_time_t now = get_absolute_time();

    if (held_key && absolute_time_diff_us(next_repeat, now) >= 0) {
        push(held_key);
        next_repeat = delayed_by_us(now, REPEAT_DELAY_US);
    }

    if (absolute_time_diff_us(next_action, now) < 0) return;

    if (phase == PH_IDLE) {
        if (pending_bkl >= 0 || pending_bk2 >= 0) {
            uint8_t rsp[2];
            if (pending_bkl >= 0) {
                reg_write(REG_BKL, (uint8_t)pending_bkl);
                pending_bkl = -1;
            } else {
                reg_write(REG_BK2, (uint8_t)pending_bk2);
                pending_bk2 = -1;
            }
            sleep_us(READ_DELAY_US);
            i2c_read_timeout_us(KBD_I2C, KBD_ADDR, rsp, 2, false, 20000);
            next_action = delayed_by_us(now, POLL_INTERVAL_US);
            return;
        }
        pending_reg = REG_FIF;
        if (absolute_time_diff_us(next_battery, now) >= 0) {
            pending_reg = REG_BAT;
            next_battery = delayed_by_us(now, BATTERY_INTERVAL_US);
        }
        if (i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &pending_reg, 1, false, 20000) != 1) {
            i2c_result(false);
            next_action = delayed_by_us(now, 100000);
            return;
        }
        phase = PH_WAIT;
        // The controller queries the PMIC for battery reads and NACKs for ~30-50ms meanwhile.
        next_action = delayed_by_us(now, pending_reg == REG_BAT ? BAT_READ_DELAY_US : READ_DELAY_US);
        return;
    }

    uint8_t buf[2] = {0, 0};
    int r = i2c_read_timeout_us(KBD_I2C, KBD_ADDR, buf, 2, false, 20000);
    if (r != 2 && pending_reg == REG_BAT && ++bat_retries < BAT_MAX_RETRIES) {
        next_action = delayed_by_us(now, 20000);
        return;
    }
    bat_retries = 0;
    phase = PH_IDLE;
    i2c_result(r == 2);
    if (r == 2) {
        if (pending_reg == REG_FIF) {
            if (buf[0] != 0 && buf[1] != 0) {
                handle_key(buf[0], buf[1]);
                // More events may be queued; poll again quickly.
                next_action = now;
                return;
            }
        } else if (pending_reg == REG_BAT) {
            battery = buf[1] & 0x7F;
            charging = (buf[1] & 0x80) != 0;
        }
    }
    next_action = delayed_by_us(now, POLL_INTERVAL_US);
}

int kbd_battery_percent(bool *chg) {
    if (chg) *chg = charging;
    // Factory keyboard firmware reports 0 when it can't read the PMU; a truly empty
    // battery would have powered the PicoCalc off, so treat 0 as "unknown".
    return battery > 0 ? battery : -1;
}

bool kbd_has_kbd_backlight(void) { return has_kbd_backlight; }

void kbd_set_lcd_backlight(uint8_t level) { pending_bkl = level > 240 ? 240 : level; }
void kbd_set_kbd_backlight(uint8_t level) { pending_bk2 = level > 240 ? 240 : level; }
