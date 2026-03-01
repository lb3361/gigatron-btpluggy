

// This is lifted from the gigatron pluggy code
// Scancodes have been translated from PS2 to HID

// Keymaps courtesy of
//    Arduino PS2Keyboard library (US,DE,FR)
//    https://playground.arduino.cc/Main/PS2Keyboard
// and
//    Teensy PS2Keyboard library (GB,IT,ES)
//    http://www.pjrc.com/teensy/td_libs_PS2Keyboard.html
//
// 229 * 3 bytes = 687 bytes for holding 6 keyboard layouts
// To save space this table excludes codes that aren't in US-ASCII
// because these aren't in the Gigatron font either (yet).
// So accented letters and such are all absent. This is needed
// to make it all fit in the ATtiny85 configuration.


#define nrKeymaps 6

enum {
  US = 1 << 0,
  GB = 1 << 1,
  DE = 1 << 2,
  FR = 1 << 3,
  IT = 1 << 4,
  ES = 1 << 5,
  KMAPS = (1 << nrKeymaps) - 1,
  NOMOD = 0 << nrKeymaps,
  SHIFT = 1 << nrKeymaps,
  ALTGR = 2 << nrKeymaps,
  EVERY = 3 << nrKeymaps,
};

typedef struct {
  uint8_t flags; // Change from byte to word if we add more keymaps
  uint8_t code;
  uint8_t ascii; // XXX Remove this if we add flags. Value can be inferred by lookup()
} keyTuple_t;

const keyTuple_t keymaps[] = {
  { +US+GB+DE+FR+IT+ES +EVERY, 0x2B,   9 }, // TAB KEY
  { +US+GB+DE+FR+IT+ES +EVERY, 0x28,  10 }, // ENTER
  { +US+GB+DE+FR+IT+ES +EVERY, 0x58,  10 }, // KP_ENTER
  { +US+GB+DE+FR+IT+ES +EVERY, 0x29,  27 }, // ESC
  { +US+GB+DE+FR+IT+ES +EVERY, 0x2C,  32 }, // SPACEBAR
  { +US+GB+DE   +IT+ES +SHIFT, 0x1E,  33 }, // '!'
  {          +FR       +NOMOD, 0x28,  33 },
  {    +GB+DE   +IT+ES +SHIFT, 0x1F,  34 }, // '"'
  {          +FR       +NOMOD, 0x20,  34 },
  { +US                +SHIFT, 0x34,  34 },
  { +US                +SHIFT, 0x20,  35 }, // '#'
  {          +FR   +ES +ALTGR, 0x20,  35 },
  {             +IT    +ALTGR, 0x34,  35 },
  {    +GB+DE          +NOMOD, 0x31,  35 },
  {       +DE+FR       +ALTGR, 0x31,  35 },
  { +US+GB+DE   +IT+ES +SHIFT, 0x21,  36 }, // '$'
  {          +FR       +NOMOD, 0x30,  36 },
  { +US+GB+DE   +IT+ES +SHIFT, 0x22,  37 }, // '%'
  {          +FR       +SHIFT, 0x34,  37 },
  {          +FR       +NOMOD, 0x1E,  38 }, // '&'
  {       +DE   +IT+ES +SHIFT, 0x23,  38 },
  { +US+GB             +SHIFT, 0x24,  38 },
  {          +FR       +NOMOD, 0x21,  39 }, // '''
  {             +IT+ES +NOMOD, 0x4D,  39 },
  { +US+GB             +NOMOD, 0x34,  39 },
  {       +DE          +NOMOD, 0x2E,  39 },
  {       +DE          +SHIFT, 0x31,  39 },
  {          +FR       +NOMOD, 0x22,  40 }, // '('
  {       +DE   +IT+ES +SHIFT, 0x25,  40 },
  { +US+GB             +SHIFT, 0x26,  40 },
  { +US+GB             +SHIFT, 0x27,  41 }, // ')'
  {       +DE   +IT+ES +SHIFT, 0x26,  41 },
  {          +FR       +NOMOD, 0x2D,  41 },
  { +US+GB             +SHIFT, 0x25,  42 }, // '*'
  {       +DE   +IT+ES +SHIFT, 0x30,  42 },
  {          +FR       +NOMOD, 0x31,  42 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x55,  42 },
  { +US+GB   +FR       +SHIFT, 0x2E,  43 }, // '+'
  {       +DE   +IT+ES +NOMOD, 0x30,  43 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x57,  43 },
  {          +FR       +NOMOD, 0x10,  44 }, // ','
  { +US+GB+DE   +IT+ES +NOMOD, 0x36,  44 },
  {          +FR       +NOMOD, 0x23,  45 }, // '-'
  {       +DE   +IT+ES +NOMOD, 0x38,  45 },
  { +US+GB             +NOMOD, 0x2D,  45 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x56,  45 },
  {          +FR       +SHIFT, 0x36,  46 }, // '.'
  { +US+GB+DE   +IT+ES +NOMOD, 0x37,  46 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x63,  46 },
  {       +DE   +IT+ES +SHIFT, 0x24,  47 }, // '/'
  {          +FR       +SHIFT, 0x37,  47 },
  { +US+GB             +NOMOD, 0x38,  47 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x54,  47 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x27,  48 }, // '0'
  {          +FR       +SHIFT, 0x27,  48 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x62,  48 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x1E,  49 }, // '1'
  {          +FR       +SHIFT, 0x1E,  49 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x59,  49 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x1F,  50 }, // '2'
  {          +FR       +SHIFT, 0x1F,  50 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x5A,  50 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x20,  51 }, // '3'
  {          +FR       +SHIFT, 0x20,  51 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x5B,  51 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x21,  52 }, // '4'
  {          +FR       +SHIFT, 0x21,  52 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x5C,  52 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x22,  53 }, // '5'
  {          +FR       +SHIFT, 0x22,  53 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x5D,  53 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x23,  54 }, // '6'
  {          +FR       +SHIFT, 0x23,  54 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x5E,  54 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x24,  55 }, // '7'
  {          +FR       +SHIFT, 0x24,  55 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x5F,  55 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x25,  56 }, // '8'
  {          +FR       +SHIFT, 0x25,  56 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x60,  56 },
  { +US+GB+DE   +IT+ES +NOMOD, 0x26,  57 }, // '9'
  {          +FR       +SHIFT, 0x26,  57 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x61,  57 },
  {          +FR       +NOMOD, 0x37,  58 }, // ':'
  {       +DE   +IT+ES +SHIFT, 0x37,  58 },
  { +US+GB             +SHIFT, 0x33,  58 },
  {          +FR       +NOMOD, 0x36,  59 }, // ';'
  {       +DE   +IT+ES +SHIFT, 0x36,  59 },
  { +US+GB             +NOMOD, 0x33,  59 },
  { +US+GB             +SHIFT, 0x36,  60 }, // '<'
  {       +DE+FR+IT+ES +NOMOD, 0x64,  60 },
  {       +DE   +IT+ES +SHIFT, 0x27,  61 }, // '='
  { +US+GB   +FR       +NOMOD, 0x2E,  61 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x67,  61 },
  { +US+GB             +SHIFT, 0x37,  62 }, // '>'
  {       +DE+FR+IT+ES +SHIFT, 0x64,  62 },
  {          +FR       +SHIFT, 0x10,  63 }, // '?'
  { +US+GB             +SHIFT, 0x38,  63 },
  {       +DE   +IT+ES +SHIFT, 0x38,  63 },
  {       +DE+FR       +ALTGR, 0x14,  64 }, // '@'
  { +US                +SHIFT, 0x1F,  64 },
  {                +ES +ALTGR, 0x1F,  64 },
  {          +FR       +ALTGR, 0x27,  64 },
  {             +IT    +ALTGR, 0x33,  64 },
  {    +GB             +SHIFT, 0x34,  64 },
  {          +FR       +SHIFT, 0x14,  65 }, // 'A'
  { +US+GB+DE   +IT+ES +SHIFT, 0x04,  65 },
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x04,  66 }, // 'B'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x06,  67 }, // 'C'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x07,  68 }, // 'D'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x08,  69 }, // 'E'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x09,  70 }, // 'F'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x0A,  71 }, // 'G'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x0B,  72 }, // 'H'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x0C,  73 }, // 'I'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x0D,  74 }, // 'J'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x0E,  75 }, // 'K'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x0F,  76 }, // 'L'
  { +US+GB+DE   +IT+ES +SHIFT, 0x10,  77 }, // 'M'
  {          +FR       +SHIFT, 0x33,  77 },
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x11,  78 }, // 'N'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x12,  79 }, // 'O'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x13,  80 }, // 'P'
  { +US+GB+DE   +IT+ES +SHIFT, 0x14,  81 }, // 'Q'
  {          +FR       +SHIFT, 0x04,  81 },
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x15,  82 }, // 'R'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x16,  83 }, // 'S'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x17,  84 }, // 'T'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x18,  85 }, // 'U'
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x19,  86 }, // 'V'
  {          +FR       +SHIFT, 0x1A,  87 }, // 'W'
  { +US+GB+DE   +IT+ES +SHIFT, 0x1D,  87 },
  { +US+GB+DE+FR+IT+ES +SHIFT, 0x1B,  88 }, // 'X'
  {       +DE          +SHIFT, 0x1D,  89 }, // 'Y'
  { +US+GB   +FR+IT+ES +SHIFT, 0x1C,  89 },
  { +US+GB      +IT+ES +SHIFT, 0x1D,  90 }, // 'Z'
  {          +FR       +SHIFT, 0x1A,  90 },
  {       +DE          +SHIFT, 0x1D,  90 },
  {          +FR       +ALTGR, 0x22,  91 }, // '['
  {       +DE   +IT    +ALTGR, 0x35,  91 },
  { +US+GB             +NOMOD, 0x2F,  91 },
  {             +IT+ES +ALTGR, 0x2F,  91 },
  {             +IT    +NOMOD, 0x35,  92 }, // '\'
  {                +ES +ALTGR, 0x35,  92 },
  {          +FR       +ALTGR, 0x25,  92 },
  {       +DE          +ALTGR, 0x2D,  92 },
  { +US                +NOMOD, 0x31,  92 },
  {    +GB             +NOMOD, 0x64,  92 },
  {       +DE   +IT    +ALTGR, 0x26,  93 }, // ']'
  {          +FR       +ALTGR, 0x4D,  93 },
  { +US+GB             +NOMOD, 0x30,  93 },
  {             +IT+ES +ALTGR, 0x30,  93 },
  {       +DE          +NOMOD, 0x35,  94 }, // '^'
  { +US+GB             +SHIFT, 0x23,  94 },
  {          +FR       +ALTGR, 0x26,  94 },
  {          +FR       +NOMOD, 0x2F,  94 },
  {                +ES +SHIFT, 0x2F,  94 },
  {             +IT    +SHIFT, 0x2E,  94 },
  {          +FR       +NOMOD, 0x25,  95 }, // '_'
  {       +DE   +IT+ES +SHIFT, 0x38,  95 },
  { +US+GB             +SHIFT, 0x38,  95 },
  { +US+GB             +NOMOD, 0x35,  96 }, // '`'
  {          +FR       +ALTGR, 0x24,  96 },
  {             +IT    +ALTGR, 0x2D,  96 },
  {                +ES +NOMOD, 0x2F,  96 },
  {       +DE          +SHIFT, 0x2E,  96 },
  {          +FR       +NOMOD, 0x14,  97 }, // 'a'
  { +US+GB+DE   +IT+ES +NOMOD, 0x04,  97 },
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x05,  98 }, // 'b'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x06,  99 }, // 'c'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x07, 100 }, // 'd'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x08, 101 }, // 'e'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x09, 102 }, // 'f'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x0A, 103 }, // 'g'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x0B, 104 }, // 'h'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x0C, 105 }, // 'i'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x0D, 106 }, // 'j'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x0E, 107 }, // 'k'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x0F, 108 }, // 'l'
  { +US+GB+DE   +IT+ES +NOMOD, 0x10, 109 }, // 'm'
  {          +FR       +NOMOD, 0x2D, 109 },
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x11, 110 }, // 'n'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x12, 111 }, // 'o'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x13, 112 }, // 'p'
  { +US+GB+DE   +IT+ES +NOMOD, 0x14, 113 }, // 'q'
  {          +FR       +NOMOD, 0x04, 113 },
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x15, 114 }, // 'r'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x16, 115 }, // 's'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x17, 116 }, // 't'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x18, 117 }, // 'u'
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x19, 118 }, // 'v'
  {          +FR       +NOMOD, 0x1D, 119 }, // 'w'
  { +US+GB+DE   +IT+ES +NOMOD, 0x1A, 119 },
  { +US+GB+DE+FR+IT+ES +NOMOD, 0x1B, 120 }, // 'x'
  {       +DE          +NOMOD, 0x1D, 121 }, // 'y'
  { +US+GB   +FR+IT+ES +NOMOD, 0x1C, 121 },
  { +US+GB      +IT+ES +NOMOD, 0x1D, 122 }, // 'z'
  {          +FR       +NOMOD, 0x1A, 122 },
  {       +DE          +NOMOD, 0x1D, 122 },
  {          +FR       +ALTGR, 0x21, 123 }, // '{'
  {       +DE   +IT    +ALTGR, 0x24, 123 },
  {                +ES +ALTGR, 0x34, 123 },
  { +US+GB             +SHIFT, 0x2F, 123 },
  {             +IT    +SHIFT, 0x35, 124 }, // '|'
  {                +ES +ALTGR, 0x1E, 124 },
  {          +FR       +ALTGR, 0x23, 124 },
  { +US                +SHIFT, 0x31, 124 },
  {    +GB             +SHIFT, 0x64, 124 },
  {       +DE+FR   +ES +ALTGR, 0x64, 124 },
  {       +DE   +IT    +ALTGR, 0x27, 125 }, // '}'
  {          +FR       +ALTGR, 0x36, 125 },       // was scancode 0x56
  { +US+GB             +SHIFT, 0x30, 125 },
  {                +ES +ALTGR, 0x31, 125 },  
  { +US                +SHIFT, 0x35, 126 }, // '~'
  {          +FR       +ALTGR, 0x1F, 126 },
  {                +ES +ALTGR, 0x21, 126 },
  {             +IT    +ALTGR, 0x2E, 126 },
  {       +DE          +ALTGR, 0x30, 126 },
  {    +GB             +SHIFT, 0x31, 126 },  
  { +US+GB+DE+FR+IT+ES +EVERY, 0x2A, 127 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x4C, 127 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x3A, 193 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x3B, 194 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x3C, 195 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x3D, 196 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x3E, 197 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x3F, 198 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x40, 199 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x41, 200 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x42, 201 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x43, 202 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x44, 203 },
  { +US+GB+DE+FR+IT+ES +EVERY, 0x45, 204 },
};


typedef struct {
  uint8_t code;
  uint8_t btn;
} keyBtn_t;

const keyBtn_t keybtns[] = {
  {0x4F,   1}, //LEFT
  {0x50,   2}, //RIGHT
  {0x51,   4}, //DOWN
  {0x52,   8}, //UP
  {0x4B,  16}, //PGUP
  {0x4E,  32}, //PGDN
  {0x49,  64}, //INS
  {0x4A,  64}, //HOME
  {0x4D, 128}, //END
  // {0x2A, 128}
  // {CTRL+ALT+0x2A, 16}
};
