// gpio_to_uinput.cpp
//
// GPIO (Linux chardev v2) -> uinput virtual GAMEPAD + (optional) KEYBOARD.
//
// - Requests GPIO lines as INPUT + PULL-UP + BOTH EDGES.
// - Treats FALLING as "press" and RISING as "release" by default (active-low buttons with pull-ups).
// - Debouncing:
//     (a) sets kernel debounce attr if supported
//     (b) ALWAYS applies userspace time-based debounce using event timestamp_ns
// - Excludes offset 36 (RP1_PCIE_CLKREQ_N) because it can be very spammy.
//
// Mapping file format (ASCII):
//   # comments allowed
//   15 HAT_UP
//   18 HAT_DOWN
//   4  HAT_LEFT
//   14 HAT_RIGHT
//   21 BTN_SOUTH
//   17 KEY_ENTER
//   22 KEY_A
//   23 BTN_START
//   ...
//
// Supported token types in the mapping:
//   - HAT_UP / HAT_DOWN / HAT_LEFT / HAT_RIGHT    -> gamepad hat ABS_HAT0X/Y (-1/0/1)
//   - BTN_* (subset listed by --list-options)     -> gamepad buttons (EV_KEY codes)
//   - KEY_* (subset + patterns listed by --list-options) -> keyboard keys (EV_KEY codes)
//   - Aliases: A..Z, 0..9, ENTER, ESC, SPACE, TAB, BACKSPACE, UP/DOWN/LEFT/RIGHT, etc (see --list-options)
//   - Numeric code: "28" -> raw EV_KEY code (sent to keyboard device)
//
// Build:
//   g++ -O2 -std=c++17 gpio_to_uinput.cpp -o gpio_to_uinput
//
// Run (Android usually needs root):
//   su -c /data/local/tmp/gpio_to_uinput --chip /dev/gpiochip0 --start 2 --end 27 --map /data/local/tmp/gpio.map --debounce-us 10000
//
// Inspect devices:
//   Linux:  evtest
//   Android: getevent -lp

#include <linux/gpio.h>
#include <linux/uinput.h>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static void die(const std::string& msg) {
  std::cerr << "ERROR: " << msg << " (errno=" << errno << " " << std::strerror(errno) << ")\n";
  std::exit(1);
}

static int xopen(const std::string& path, int flags) {
  int fd = ::open(path.c_str(), flags);
  if (fd < 0) die("open(" + path + ")");
  return fd;
}

static void set_nonblock(int fd) {
  int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl < 0) die("fcntl(F_GETFL)");
  if (::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) die("fcntl(F_SETFL)");
}

static bool is_excluded(uint32_t off) {
  return off == 36;
}

static std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static std::string upper(std::string s) {
  for (char& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}

static bool is_all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
  return true;
}

static std::optional<gpio_v2_line_info> get_line_info(int chip_fd, uint32_t offset) {
  gpio_v2_line_info info;
  std::memset(&info, 0, sizeof(info));
  info.offset = offset;
  if (::ioctl(chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, &info) < 0) return std::nullopt;
  return info;
}

static std::optional<int> request_line(int chip_fd, uint32_t offset,
                                       uint32_t event_buf_sz,
                                       uint32_t debounce_us) {
  gpio_v2_line_request req;
  std::memset(&req, 0, sizeof(req));
  req.offsets[0] = offset;
  req.num_lines = 1;
  req.event_buffer_size = event_buf_sz;
  std::snprintf(req.consumer, sizeof(req.consumer), "gpio_to_uinput");

  // INPUT + PULL-UP + BOTH EDGES
  req.config.flags =
      GPIO_V2_LINE_FLAG_INPUT |
      GPIO_V2_LINE_FLAG_BIAS_PULL_UP |
      GPIO_V2_LINE_FLAG_EDGE_RISING |
      GPIO_V2_LINE_FLAG_EDGE_FALLING;

  // Kernel debounce (if supported by kernel/driver).
  if (debounce_us > 0) {
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
    req.config.attrs[0].attr.debounce_period_us = debounce_us;
    req.config.attrs[0].mask = 1ULL;
  }

  if (::ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) return std::nullopt;
  if (req.fd < 0) return std::nullopt;

  set_nonblock(req.fd);
  return req.fd;
}

// --- uinput helpers ---

static void uinput_emit(int ufd, uint16_t type, uint16_t code, int32_t value) {
  input_event ev;
  std::memset(&ev, 0, sizeof(ev));
  timeval tv{};
  gettimeofday(&tv, nullptr);
  ev.time = tv;
  ev.type = type;
  ev.code = code;
  ev.value = value;
  if (::write(ufd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) die("write(uinput event)");
}

static void uinput_syn(int ufd) {
  uinput_emit(ufd, EV_SYN, SYN_REPORT, 0);
}

static void uinput_key(int ufd, int code, bool down) {
  uinput_emit(ufd, EV_KEY, (uint16_t)code, down ? 1 : 0);
  uinput_syn(ufd);
}

static void uinput_abs(int ufd, uint16_t code, int32_t value) {
  uinput_emit(ufd, EV_ABS, code, value);
}

// --- Mapping / Actions ---

enum class DeviceKind { Gamepad, Keyboard };
enum class ActionType { ButtonOrKey, HatDir };
enum class HatDir { Up, Down, Left, Right };

struct Action {
  ActionType type;
  DeviceKind dev;     // for ButtonOrKey: which uinput device to send to
  int code;           // EV_KEY code for ButtonOrKey
  HatDir hat_dir;     // for HatDir
  std::string token;  // original token for logging
};

// Buttons we explicitly support by name (still can use numeric fallback).
static const std::pair<const char*, int> kBtnTable[] = {
  {"BTN_SOUTH",  BTN_SOUTH},
  {"BTN_EAST",   BTN_EAST},
  {"BTN_NORTH",  BTN_NORTH},
  {"BTN_WEST",   BTN_WEST},
  {"BTN_TL",     BTN_TL},
  {"BTN_TR",     BTN_TR},
  {"BTN_TL2",    BTN_TL2},
  {"BTN_TR2",    BTN_TR2},
  {"BTN_SELECT", BTN_SELECT},
  {"BTN_START",  BTN_START},
  {"BTN_MODE",   BTN_MODE},
  {"BTN_THUMBL", BTN_THUMBL},
  {"BTN_THUMBR", BTN_THUMBR},
  {"BTN_DPAD_UP",    BTN_DPAD_UP},
  {"BTN_DPAD_DOWN",  BTN_DPAD_DOWN},
  {"BTN_DPAD_LEFT",  BTN_DPAD_LEFT},
  {"BTN_DPAD_RIGHT", BTN_DPAD_RIGHT},
  {"BTN_GAMEPAD", BTN_GAMEPAD},
};

// A “complete list for this tool” of named keyboard keys (plus patterns below).
static const std::pair<const char*, int> kKeyTable[] = {
  {"KEY_ENTER", KEY_ENTER},
  {"KEY_ESC", KEY_ESC},
  {"KEY_TAB", KEY_TAB},
  {"KEY_SPACE", KEY_SPACE},
  {"KEY_BACKSPACE", KEY_BACKSPACE},
  {"KEY_LEFTCTRL", KEY_LEFTCTRL},
  {"KEY_RIGHTCTRL", KEY_RIGHTCTRL},
  {"KEY_LEFTSHIFT", KEY_LEFTSHIFT},
  {"KEY_RIGHTSHIFT", KEY_RIGHTSHIFT},
  {"KEY_LEFTALT", KEY_LEFTALT},
  {"KEY_RIGHTALT", KEY_RIGHTALT},
  {"KEY_LEFTMETA", KEY_LEFTMETA},
  {"KEY_RIGHTMETA", KEY_RIGHTMETA},
  {"KEY_CAPSLOCK", KEY_CAPSLOCK},

  {"KEY_UP", KEY_UP},
  {"KEY_DOWN", KEY_DOWN},
  {"KEY_LEFT", KEY_LEFT},
  {"KEY_RIGHT", KEY_RIGHT},
  {"KEY_HOME", KEY_HOME},
  {"KEY_END", KEY_END},
  {"KEY_PAGEUP", KEY_PAGEUP},
  {"KEY_PAGEDOWN", KEY_PAGEDOWN},
  {"KEY_INSERT", KEY_INSERT},
  {"KEY_DELETE", KEY_DELETE},

  {"KEY_MINUS", KEY_MINUS},
  {"KEY_EQUAL", KEY_EQUAL},
  {"KEY_LEFTBRACE", KEY_LEFTBRACE},
  {"KEY_RIGHTBRACE", KEY_RIGHTBRACE},
  {"KEY_BACKSLASH", KEY_BACKSLASH},
  {"KEY_SEMICOLON", KEY_SEMICOLON},
  {"KEY_APOSTROPHE", KEY_APOSTROPHE},
  {"KEY_GRAVE", KEY_GRAVE},
  {"KEY_COMMA", KEY_COMMA},
  {"KEY_DOT", KEY_DOT},
  {"KEY_SLASH", KEY_SLASH},

  {"KEY_SYSRQ", KEY_SYSRQ},
  {"KEY_PAUSE", KEY_PAUSE},
  {"KEY_SCROLLLOCK", KEY_SCROLLLOCK},
  {"KEY_NUMLOCK", KEY_NUMLOCK},
  {"KEY_PRINT", KEY_PRINT},

  {"KEY_VOLUMEUP", KEY_VOLUMEUP},
  {"KEY_VOLUMEDOWN", KEY_VOLUMEDOWN},
  {"KEY_MUTE", KEY_MUTE},
  {"KEY_PLAYPAUSE", KEY_PLAYPAUSE},
  {"KEY_NEXTSONG", KEY_NEXTSONG},
  {"KEY_PREVIOUSSONG", KEY_PREVIOUSSONG},
  {"KEY_STOPCD", KEY_STOPCD},
};

static std::optional<int> lookup_table_code(const std::pair<const char*, int>* table, size_t n, const std::string& s) {
  for (size_t i = 0; i < n; i++) {
    if (s == table[i].first) return table[i].second;
  }
  return std::nullopt;
}

// Patterns supported for keyboard keys:
//   KEY_A..KEY_Z
//   KEY_0..KEY_9
//   KEY_F1..KEY_F24
//   KEY_KP0..KEY_KP9
static std::optional<int> keycode_from_string(std::string s) {
  s = upper(trim(s));
  if (s.empty()) return std::nullopt;

  // numeric = raw EV_KEY code
  if (is_all_digits(s)) return std::stoi(s);

  // normalize common aliases
  if (s == "ENTER") s = "KEY_ENTER";
  if (s == "ESC") s = "KEY_ESC";
  if (s == "SPACE") s = "KEY_SPACE";
  if (s == "TAB") s = "KEY_TAB";
  if (s == "BACKSPACE") s = "KEY_BACKSPACE";
  if (s == "UP") s = "KEY_UP";
  if (s == "DOWN") s = "KEY_DOWN";
  if (s == "LEFT") s = "KEY_LEFT";
  if (s == "RIGHT") s = "KEY_RIGHT";

  // Single-letter alias -> KEY_A..KEY_Z
  if (s.size() == 1 && s[0] >= 'A' && s[0] <= 'Z') {
    return KEY_A + (s[0] - 'A');
  }
  // Single-digit alias -> KEY_0..KEY_9
  if (s.size() == 1 && s[0] >= '0' && s[0] <= '9') {
    if (s[0] == '0') return KEY_0;
    return KEY_1 + (s[0] - '1');
  }

  // Direct table lookup
  if (auto kc = lookup_table_code(kKeyTable, sizeof(kKeyTable) / sizeof(kKeyTable[0]), s)) return kc;

  // Pattern: KEY_A..KEY_Z or KEY_0..KEY_9
  if (s.rfind("KEY_", 0) == 0) {
    std::string tail = s.substr(4);

    if (tail.size() == 1 && tail[0] >= 'A' && tail[0] <= 'Z') {
      return KEY_A + (tail[0] - 'A');
    }
    if (tail.size() == 1 && tail[0] >= '0' && tail[0] <= '9') {
      if (tail[0] == '0') return KEY_0;
      return KEY_1 + (tail[0] - '1');
    }

    // KEY_F1..KEY_F24
    if (tail.size() >= 2 && tail[0] == 'F' && is_all_digits(tail.substr(1))) {
      int f = std::stoi(tail.substr(1));
      if (f >= 1 && f <= 24) return KEY_F1 + (f - 1);
    }

    // KEY_KP0..KEY_KP9
    if (tail.size() == 3 && tail.rfind("KP", 0) == 0 && std::isdigit((unsigned char)tail[2])) {
      int d = tail[2] - '0';
      return KEY_KP0 + d;
    }
  }

  return std::nullopt;
}

static std::optional<int> btncode_from_string(std::string s) {
  s = upper(trim(s));
  if (s.empty()) return std::nullopt;

  // numeric = raw EV_KEY code (advanced; goes to gamepad device if you force BTN_NUMERIC via map)
  if (is_all_digits(s)) return std::stoi(s);

  // sugar aliases for common gamepad face buttons
  if (s == "A") s = "BTN_SOUTH";
  if (s == "B") s = "BTN_EAST";
  if (s == "X") s = "BTN_WEST";
  if (s == "Y") s = "BTN_NORTH";
  if (s == "START") s = "BTN_START";
  if (s == "SELECT") s = "BTN_SELECT";

  if (auto bc = lookup_table_code(kBtnTable, sizeof(kBtnTable) / sizeof(kBtnTable[0]), s)) return bc;
  return std::nullopt;
}

static std::optional<Action> action_from_token(std::string tok) {
  tok = upper(trim(tok));
  if (tok.empty()) return std::nullopt;

  // Hat
  if (tok == "HAT_UP")    return Action{ActionType::HatDir, DeviceKind::Gamepad, 0, HatDir::Up, tok};
  if (tok == "HAT_DOWN")  return Action{ActionType::HatDir, DeviceKind::Gamepad, 0, HatDir::Down, tok};
  if (tok == "HAT_LEFT")  return Action{ActionType::HatDir, DeviceKind::Gamepad, 0, HatDir::Left, tok};
  if (tok == "HAT_RIGHT") return Action{ActionType::HatDir, DeviceKind::Gamepad, 0, HatDir::Right, tok};

  // Explicit BTN_* -> gamepad EV_KEY
  if (tok.rfind("BTN_", 0) == 0 || tok == "A" || tok == "B" || tok == "X" || tok == "Y" || tok == "START" || tok == "SELECT") {
    auto bc = btncode_from_string(tok);
    if (!bc) return std::nullopt;
    return Action{ActionType::ButtonOrKey, DeviceKind::Gamepad, *bc, HatDir::Up, tok};
  }

  // Everything else: treat as keyboard token (KEY_* names, aliases, numeric raw code)
  auto kc = keycode_from_string(tok);
  if (!kc) return std::nullopt;
  return Action{ActionType::ButtonOrKey, DeviceKind::Keyboard, *kc, HatDir::Up, tok};
}

static std::unordered_map<uint32_t, Action> load_mapping_file(const std::string& path) {
  std::unordered_map<uint32_t, Action> m;
  std::ifstream in(path);
  if (!in) die("open map file: " + path);

  std::string line;
  int ln = 0;
  while (std::getline(in, line)) {
    ln++;
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;

    for (char& c : line) if (c == ':') c = ' ';
    std::istringstream iss(line);

    std::string gpio_s, tok;
    if (!(iss >> gpio_s >> tok)) {
      std::cerr << "WARN: bad map line " << ln << ": " << line << "\n";
      continue;
    }

    uint32_t off = (uint32_t)std::stoul(gpio_s);
    auto act = action_from_token(tok);
    if (!act) {
      std::cerr << "WARN: unknown token '" << tok << "' on line " << ln << "\n";
      continue;
    }
    m[off] = *act;
  }
  return m;
}

// Defaults based on your earlier log: arrows -> hat, enter -> BTN_SOUTH (A)
static std::unordered_map<uint32_t, Action> default_mapping_from_your_log() {
  std::unordered_map<uint32_t, Action> m;
  m[15] = *action_from_token("HAT_UP");
  m[18] = *action_from_token("HAT_DOWN");
  m[4]  = *action_from_token("HAT_LEFT");
  m[14] = *action_from_token("HAT_RIGHT");
  m[21] = *action_from_token("BTN_SOUTH");
  return m;
}

// --- uinput device creation ---

static void setup_abs_hat(int ufd, uint16_t code) {
  if (ioctl(ufd, UI_SET_ABSBIT, code) < 0) die("UI_SET_ABSBIT");
  uinput_abs_setup abs{};
  abs.code = code;
  abs.absinfo.minimum = -1;
  abs.absinfo.maximum =  1;
  abs.absinfo.fuzz = 0;
  abs.absinfo.flat = 0;
  abs.absinfo.resolution = 0;
  if (ioctl(ufd, UI_ABS_SETUP, &abs) < 0) {
    std::cerr << "WARN: UI_ABS_SETUP failed for ABS code " << code
              << " (errno=" << errno << " " << std::strerror(errno) << ")\n";
  }
}

static int create_uinput_gamepad(const std::set<int>& button_codes, bool need_hat) {
  int ufd = xopen("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

  if (ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0) die("UI_SET_EVBIT EV_KEY");
  if (ioctl(ufd, UI_SET_EVBIT, EV_SYN) < 0) die("UI_SET_EVBIT EV_SYN");

  if (need_hat) {
    if (ioctl(ufd, UI_SET_EVBIT, EV_ABS) < 0) die("UI_SET_EVBIT EV_ABS");
    setup_abs_hat(ufd, ABS_HAT0X);
    setup_abs_hat(ufd, ABS_HAT0Y);
  }

  // Marker: many stacks like seeing BTN_GAMEPAD.
  (void)ioctl(ufd, UI_SET_KEYBIT, BTN_GAMEPAD);

  for (int b : button_codes) {
    if (ioctl(ufd, UI_SET_KEYBIT, b) < 0) {
      std::cerr << "WARN: UI_SET_KEYBIT failed for " << b
                << " (errno=" << errno << " " << std::strerror(errno) << ")\n";
    }
  }

  uinput_setup usetup{};
  std::snprintf(usetup.name, sizeof(usetup.name), "gpio-virtual-gamepad");
  usetup.id.bustype = BUS_USB;
  usetup.id.vendor  = 0x18D1;
  usetup.id.product = 0x0001;
  usetup.id.version = 1;

  if (ioctl(ufd, UI_DEV_SETUP, &usetup) < 0) die("UI_DEV_SETUP (gamepad)");
  if (ioctl(ufd, UI_DEV_CREATE) < 0) die("UI_DEV_CREATE (gamepad)");

  usleep(100 * 1000);

  if (need_hat) {
    uinput_abs(ufd, ABS_HAT0X, 0);
    uinput_abs(ufd, ABS_HAT0Y, 0);
    uinput_syn(ufd);
  }

  return ufd;
}

static int create_uinput_keyboard(const std::set<int>& key_codes) {
  int ufd = xopen("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

  if (ioctl(ufd, UI_SET_EVBIT, EV_KEY) < 0) die("UI_SET_EVBIT EV_KEY");
  if (ioctl(ufd, UI_SET_EVBIT, EV_SYN) < 0) die("UI_SET_EVBIT EV_SYN");

  for (int kc : key_codes) {
    if (ioctl(ufd, UI_SET_KEYBIT, kc) < 0) {
      std::cerr << "WARN: UI_SET_KEYBIT failed for " << kc
                << " (errno=" << errno << " " << std::strerror(errno) << ")\n";
    }
  }

  uinput_setup usetup{};
  std::snprintf(usetup.name, sizeof(usetup.name), "gpio-virtual-keyboard");
  usetup.id.bustype = BUS_USB;
  usetup.id.vendor  = 0x18D1;
  usetup.id.product = 0x0002;
  usetup.id.version = 1;

  if (ioctl(ufd, UI_DEV_SETUP, &usetup) < 0) die("UI_DEV_SETUP (keyboard)");
  if (ioctl(ufd, UI_DEV_CREATE) < 0) die("UI_DEV_CREATE (keyboard)");

  usleep(100 * 1000);
  return ufd;
}

// Auto-mapping mode for GPIOs not mentioned in the map file.
enum class AutoMode { Buttons, Keys, None };

static int next_auto_button_code(int idx) {
  static const int kList[] = {
    BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
    BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
    BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR, BTN_MODE
  };
  if (idx < (int)(sizeof(kList) / sizeof(kList[0]))) return kList[idx];
  int j = idx - (int)(sizeof(kList) / sizeof(kList[0]));
  if (j >= 0 && j <= 9) return BTN_0 + j;
  return BTN_0 + (j % 10);
}

static int next_auto_key_code(int idx) {
  // KEY_A..KEY_Z then KEY_1..KEY_0 then KEY_F1..KEY_F12
  if (idx < 26) return KEY_A + idx;
  idx -= 26;
  if (idx < 10) {
    int d = idx; // 0..9
    if (d == 0) return KEY_0;
    return KEY_1 + (d - 1);
  }
  idx -= 10;
  if (idx < 12) return KEY_F1 + idx;
  // fallback: keep cycling letters
  return KEY_A + (idx % 26);
}

static void print_options_and_exit() {
  std::cout
    << "Valid mapping tokens for this program:\n\n"
    << "HAT (gamepad hat switch):\n"
    << "  HAT_UP, HAT_DOWN, HAT_LEFT, HAT_RIGHT\n\n"
    << "BTN_* (gamepad buttons supported by name):\n";
  for (const auto& p : kBtnTable) std::cout << "  " << p.first << "\n";

  std::cout
    << "\nKEY_* (keyboard keys supported by name):\n";
  for (const auto& p : kKeyTable) std::cout << "  " << p.first << "\n";

  std::cout
    << "\nKEY_* patterns supported:\n"
    << "  KEY_A .. KEY_Z\n"
    << "  KEY_0 .. KEY_9\n"
    << "  KEY_F1 .. KEY_F24\n"
    << "  KEY_KP0 .. KEY_KP9\n\n"
    << "Aliases (keyboard):\n"
    << "  A..Z, 0..9, ENTER, ESC, SPACE, TAB, BACKSPACE, UP, DOWN, LEFT, RIGHT\n\n"
    << "Numeric raw code (keyboard by default):\n"
    << "  e.g. 28   (sends EV_KEY code 28 on the keyboard device)\n\n";
  std::exit(0);
}

struct WatchedLine {
  int req_fd;
  uint32_t offset;
  std::string name;
};

int main(int argc, char** argv) {
  std::string chip_path = "/dev/gpiochip0";
  uint32_t start = 2;
  uint32_t end = 27;
  uint32_t debounce_us = 10000;
  uint32_t event_buf_sz = 256;
  std::string map_path;

  bool active_low = true;
  AutoMode auto_mode = AutoMode::Buttons;

  {
    sched_param sp{};
    int prio = sched_get_priority_max(SCHED_FIFO);
    if (prio < 1) prio = 1;
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
      std::cerr << "WARN: failed to set SCHED_FIFO priority (errno="
                << errno << " " << std::strerror(errno) << ")\n";
    }
  }

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](const char* what) -> std::string {
      if (i + 1 >= argc) die(std::string("missing value for ") + what);
      return argv[++i];
    };

    if (a == "--chip") chip_path = need("--chip");
    else if (a == "--start") start = (uint32_t)std::stoul(need("--start"));
    else if (a == "--end") end = (uint32_t)std::stoul(need("--end"));
    else if (a == "--debounce-us") debounce_us = (uint32_t)std::stoul(need("--debounce-us"));
    else if (a == "--event-buf") event_buf_sz = (uint32_t)std::stoul(need("--event-buf"));
    else if (a == "--map") map_path = need("--map");
    else if (a == "--active-high") active_low = false;
    else if (a == "--auto") {
      std::string v = upper(trim(need("--auto")));
      if (v == "BUTTONS") auto_mode = AutoMode::Buttons;
      else if (v == "KEYS") auto_mode = AutoMode::Keys;
      else if (v == "NONE") auto_mode = AutoMode::None;
      else die("bad --auto value (use buttons|keys|none)");
    } else if (a == "--list-options") {
      print_options_and_exit();
    } else {
      std::cerr
        << "Usage:\n"
        << "  " << argv[0] << " [--chip /dev/gpiochipN] [--start N] [--end N]\n"
        << "             [--debounce-us N] [--event-buf N] [--map path] [--active-high]\n"
        << "             [--auto buttons|keys|none] [--list-options]\n\n"
        << "Defaults: chip=/dev/gpiochip0 start=2 end=27 debounce-us=10000 auto=buttons\n";
      return 2;
    }
  }

  int chip_fd = xopen(chip_path, O_RDONLY | O_CLOEXEC);

  gpiochip_info cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));
  if (ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &cinfo) < 0) die("GPIO_GET_CHIPINFO_IOCTL");

  if (end >= cinfo.lines) end = cinfo.lines - 1;

  // Build mapping.
  std::unordered_map<uint32_t, Action> map =
      map_path.empty() ? default_mapping_from_your_log() : load_mapping_file(map_path);

  // Auto-assign unmapped offsets in range.
  std::vector<uint32_t> candidates;
  for (uint32_t off = start; off <= end; off++) {
    if (is_excluded(off)) continue;
    candidates.push_back(off);
  }
  std::sort(candidates.begin(), candidates.end());

  std::set<int> used_btn, used_key;
  for (const auto& kv : map) {
    const Action& a = kv.second;
    if (a.type == ActionType::ButtonOrKey) {
      if (a.dev == DeviceKind::Gamepad) used_btn.insert(a.code);
      else used_key.insert(a.code);
    }
  }

  int auto_idx = 0;
  for (uint32_t off : candidates) {
    if (map.find(off) != map.end()) continue;
    if (auto_mode == AutoMode::None) continue;

    if (auto_mode == AutoMode::Buttons) {
      int code;
      for (;;) {
        code = next_auto_button_code(auto_idx++);
        if (used_btn.insert(code).second) break;
        if (auto_idx > 2000) break;
      }
      map[off] = Action{ActionType::ButtonOrKey, DeviceKind::Gamepad, code, HatDir::Up, "AUTO_BTN"};
    } else {
      int code;
      for (;;) {
        code = next_auto_key_code(auto_idx++);
        if (used_key.insert(code).second) break;
        if (auto_idx > 2000) break;
      }
      map[off] = Action{ActionType::ButtonOrKey, DeviceKind::Keyboard, code, HatDir::Up, "AUTO_KEY"};
    }
  }

  // Request GPIO lines (skip used/consumer/output).
  std::vector<WatchedLine> watched;
  watched.reserve(map.size());

  for (const auto& kv : map) {
    uint32_t off = kv.first;
    if (off < start || off > end) continue;
    if (is_excluded(off)) continue;

    auto infoOpt = get_line_info(chip_fd, off);
    if (!infoOpt) continue;
    auto info = *infoOpt;

    bool used_flag = (info.flags & GPIO_V2_LINE_FLAG_USED);
    bool is_output = (info.flags & GPIO_V2_LINE_FLAG_OUTPUT);
    bool has_consumer = (info.consumer[0] != '\0');
    if (used_flag || has_consumer || is_output) continue;

    auto fdOpt = request_line(chip_fd, off, event_buf_sz, debounce_us);
    if (!fdOpt) continue;

    std::string lname = (info.name[0] ? info.name : "");
    watched.push_back(WatchedLine{*fdOpt, off, lname});
  }

  if (watched.empty()) {
    std::cerr << "No lines could be requested.\n"
              << "On Android: run as root, and ensure /dev/gpiochip* and /dev/uinput are accessible.\n";
    return 1;
  }

  // Determine needed uinput devices and capabilities.
  bool need_gamepad = false;
  bool need_keyboard = false;
  bool need_hat = false;

  std::set<int> gamepad_buttons;
  std::set<int> keyboard_keys;

  for (const auto& kv : map) {
    const Action& a = kv.second;
    if (a.type == ActionType::HatDir) {
      need_gamepad = true;
      need_hat = true;
    } else {
      if (a.dev == DeviceKind::Gamepad) {
        need_gamepad = true;
        gamepad_buttons.insert(a.code);
      } else {
        need_keyboard = true;
        keyboard_keys.insert(a.code);
      }
    }
  }

  int ufd_gamepad = -1;
  int ufd_keyboard = -1;

  if (need_gamepad) ufd_gamepad = create_uinput_gamepad(gamepad_buttons, need_hat);
  if (need_keyboard) ufd_keyboard = create_uinput_keyboard(keyboard_keys);

  std::cerr << "Watching " << watched.size() << " GPIO lines.\n";
  std::cerr << "Active " << (active_low ? "LOW (FALLING=press)" : "HIGH (RISING=press)") << "\n";
  std::cerr << "Debounce: " << debounce_us << " us (kernel attr if supported + userspace filter)\n";
  if (need_gamepad) std::cerr << "Gamepad device: enabled (hat=" << (need_hat ? "yes" : "no") << ")\n";
  if (need_keyboard) std::cerr << "Keyboard device: enabled\n";

  // Userspace debounce state: last accepted event timestamp per offset
  const uint64_t debounce_ns = (uint64_t)debounce_us * 1000ULL;
  std::unordered_map<uint32_t, uint64_t> last_accept_ns;

  // Hat state (pressed directions)
  bool hat_up=false, hat_down=false, hat_left=false, hat_right=false;
  int last_hat_x = 0, last_hat_y = 0;

  auto recompute_hat = [&]() {
    if (ufd_gamepad < 0 || !need_hat) return;
    int x = (hat_right ? 1 : 0) + (hat_left ? -1 : 0);
    int y = (hat_down  ? 1 : 0) + (hat_up   ? -1 : 0);
    x = std::max(-1, std::min(1, x));
    y = std::max(-1, std::min(1, y));
    if (x != last_hat_x || y != last_hat_y) {
      uinput_abs(ufd_gamepad, ABS_HAT0X, x);
      uinput_abs(ufd_gamepad, ABS_HAT0Y, y);
      uinput_syn(ufd_gamepad);
      last_hat_x = x;
      last_hat_y = y;
    }
  };

  std::vector<pollfd> pfds(watched.size());
  for (size_t i = 0; i < watched.size(); i++) {
    pfds[i].fd = watched[i].req_fd;
    pfds[i].events = POLLIN;
    pfds[i].revents = 0;
  }

  std::vector<gpio_v2_line_event> evbuf(128);

  while (true) {
    int r = poll(pfds.data(), pfds.size(), -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      die("poll()");
    }

    for (size_t i = 0; i < pfds.size(); i++) {
      if (!(pfds[i].revents & POLLIN)) continue;

      while (true) {
        ssize_t n = read(pfds[i].fd, evbuf.data(), evbuf.size() * sizeof(gpio_v2_line_event));
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          die("read(gpio event)");
        }
        if (n == 0) break;

        size_t cnt = (size_t)n / sizeof(gpio_v2_line_event);
        for (size_t k = 0; k < cnt; k++) {
          const auto& e = evbuf[k];
          uint32_t off = e.offset;

          auto it = map.find(off);
          if (it == map.end()) continue;

          bool is_rising  = (e.id == GPIO_V2_LINE_EVENT_RISING_EDGE);
          bool is_falling = (e.id == GPIO_V2_LINE_EVENT_FALLING_EDGE);
          if (!is_rising && !is_falling) continue;

          // Userspace debounce: drop edges too close together on the same GPIO.
          uint64_t ts = e.timestamp_ns;
          auto lt = last_accept_ns.find(off);
          if (debounce_ns > 0 && lt != last_accept_ns.end()) {
            if (ts >= lt->second && (ts - lt->second) < debounce_ns) {
              continue;
            }
          }
          last_accept_ns[off] = ts;

          bool press = active_low ? is_falling : is_rising;

          const Action& act = it->second;

          if (act.type == ActionType::HatDir) {
            switch (act.hat_dir) {
              case HatDir::Up:    hat_up    = press; break;
              case HatDir::Down:  hat_down  = press; break;
              case HatDir::Left:  hat_left  = press; break;
              case HatDir::Right: hat_right = press; break;
            }
            recompute_hat();
          } else {
            int outfd = (act.dev == DeviceKind::Gamepad) ? ufd_gamepad : ufd_keyboard;
            if (outfd >= 0) uinput_key(outfd, act.code, press);
          }

          // Log
          std::string nm("-");
          for (const auto& L : watched) {
            if (L.offset == off && !L.name.empty()) { nm = L.name; break; }
          }

          std::cout << "t_ns=" << ts
                    << " offset=" << off
                    << " name=" << nm
                    << " token=" << act.token
                    << " -> " << (press ? "DOWN" : "UP");

          if (act.type == ActionType::HatDir) {
            std::cout << " (hat x=" << last_hat_x << " y=" << last_hat_y << ")";
          } else {
            std::cout << " (dev=" << (act.dev == DeviceKind::Gamepad ? "gamepad" : "keyboard")
                      << " code=" << act.code << ")";
          }

          std::cout << "\n";
          std::cout.flush();
        }
      }
    }
  }

  return 0;
}
