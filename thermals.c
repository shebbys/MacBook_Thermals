// thermals: live terminal graph + CSV log of Apple Silicon temperatures.
// Reads the SMC and IOKit HID sensors directly. No sudo, no dependencies.
// Build: make     Run: ./thermals [-i secs] [-o file.csv] [-q] [-1] [-a]
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <notify.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- SMC ---------- */
typedef struct { char major, minor, build, reserved; uint16_t release; } SMCVers;
typedef struct { uint16_t version, length; uint32_t cpu, gpu, mem; } SMCPLimit;
typedef struct { uint32_t dataSize, dataType; char dataAttributes; } SMCKeyInfo;
typedef struct { uint32_t key; SMCVers vers; SMCPLimit p; SMCKeyInfo info;
                 char result, status, cmd; uint32_t data32; unsigned char bytes[32]; } SMCParam;
static io_connect_t smc;
static int smc_call(SMCParam *in, SMCParam *out) {
  size_t n = sizeof *out;
  return IOConnectCallStructMethod(smc, 2, in, sizeof *in, out, &n);
}
static uint32_t fourcc(const char *s) { return (uint32_t)s[0]<<24 | s[1]<<16 | s[2]<<8 | s[3]; }

#define MAXK 1024
static uint32_t keys[MAXK]; static SMCKeyInfo kinfo[MAXK]; static int nkeys;

static void smc_open(void) {
  io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
  if (!s || IOServiceOpen(s, mach_task_self(), 0, &smc)) { fprintf(stderr, "can't open AppleSMC\n"); exit(1); }
  SMCParam in = {0}, out = {0};
  in.key = fourcc("#KEY"); in.cmd = 9; smc_call(&in, &out);
  in.info = out.info; in.cmd = 5; smc_call(&in, &out);
  uint32_t total = (uint32_t)out.bytes[0]<<24 | out.bytes[1]<<16 | out.bytes[2]<<8 | out.bytes[3];
  for (uint32_t i = 0; i < total && nkeys < MAXK; i++) {      // keep float keys for temps, fans, power
    memset(&in, 0, sizeof in); in.cmd = 8; in.data32 = i; smc_call(&in, &out);
    uint32_t k = out.key; char c = (char)(k >> 24);
    if (c != 'T' && c != 'F' && c != 'P') continue;
    memset(&in, 0, sizeof in); in.key = k; in.cmd = 9; smc_call(&in, &out);
    if (out.info.dataType != fourcc("flt ")) continue;
    keys[nkeys] = k; kinfo[nkeys++] = out.info;
  }
}
static float smc_read_idx(int i) {
  SMCParam in = {0}, out = {0};
  in.key = keys[i]; in.info = kinfo[i]; in.cmd = 5;
  if (smc_call(&in, &out) || out.result) return -1;
  float f; memcpy(&f, out.bytes, 4); return f;
}
static float smc_get(const char *name) {
  uint32_t k = fourcc(name);
  for (int i = 0; i < nkeys; i++) if (keys[i] == k) return smc_read_idx(i);
  return -1;
}
// Average and max of all sane temperature keys starting with a 2-char prefix.
static void smc_group(const char *pfx, float *avg, float *max) {
  double sum = 0; int n = 0; float mx = -1;
  for (int i = 0; i < nkeys; i++) {
    if ((char)(keys[i]>>24) != pfx[0] || (char)(keys[i]>>16) != pfx[1]) continue;
    float t = smc_read_idx(i);
    if (t < 5 || t > 130) continue;
    sum += t; n++; if (t > mx) mx = t;
  }
  *avg = n ? (float)(sum / n) : -1; *max = mx;
}

/* ---------- IOKit HID temperature sensors (CPU dies, SSD, battery) ---------- */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef);
int IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef, CFDictionaryRef);
CFArrayRef IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef);
IOHIDEventRef IOHIDServiceClientCopyEvent(IOHIDServiceClientRef, int64_t, int32_t, int64_t);
CFTypeRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef, CFStringRef);
double IOHIDEventGetFloatValue(IOHIDEventRef, int32_t);
static IOHIDEventSystemClientRef hid;

static void hid_open(void) {
  int page = 0xff00, usage = 5;
  CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &page), u = CFNumberCreate(NULL, kCFNumberIntType, &usage);
  const void *k[] = {CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage")}, *v[] = {p, u};
  CFDictionaryRef m = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  hid = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
  IOHIDEventSystemClientSetMatching(hid, m);
}
// For each HID sensor whose name starts with prefix: average and max.
typedef void (*hid_cb)(const char *name, double t, void *ctx);
static void hid_each(hid_cb cb, void *ctx) {
  CFArrayRef svcs = IOHIDEventSystemClientCopyServices(hid);
  if (!svcs) return;
  for (CFIndex i = 0; i < CFArrayGetCount(svcs); i++) {
    IOHIDServiceClientRef s = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(svcs, i);
    CFStringRef name = IOHIDServiceClientCopyProperty(s, CFSTR("Product"));
    IOHIDEventRef ev = IOHIDServiceClientCopyEvent(s, 15, 0, 0);
    if (name && ev) {
      char buf[128]; CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
      double t = IOHIDEventGetFloatValue(ev, 15 << 16);
      if (t > 0 && t < 150) cb(buf, t, ctx);
    }
    if (ev) CFRelease(ev);
    if (name) CFRelease(name);
  }
  CFRelease(svcs);
}
typedef struct { const char *pfx; double sum, max; int n; } Agg;
static void agg_cb(const char *name, double t, void *c) {
  Agg *a = c; if (strncmp(name, a->pfx, strlen(a->pfx))) return;
  a->sum += t; a->n++; if (t > a->max) a->max = t;
}
static void hid_group(const char *pfx, float *avg, float *max) {
  Agg a = {pfx, 0, -1, 0}; hid_each(agg_cb, &a);
  *avg = a.n ? (float)(a.sum / a.n) : -1; *max = (float)a.max;
}
static void print_cb(const char *name, double t, void *c) { (void)c; printf("  HID %-24s %6.1f °C\n", name, t); }

/* ---------- thermal pressure (what macOS itself thinks) ---------- */
static const char *pressure(void) {
  static int tok = -1; uint64_t st = 0;
  if (tok < 0 && notify_register_check("com.apple.system.thermalpressurelevel", &tok)) return "?";
  notify_get_state(tok, &st);
  switch (st) { case 0: return "nominal"; case 1: return "moderate"; case 2: return "heavy";
                case 3: return "trapping"; case 4: return "sleeping"; default: return "?"; }
}

/* ---------- sample ---------- */
enum { CPU, CPUMAX, GPU, GPUMAX, MEM, SSD, BATT, FAN0, FAN1, WATTS, NF };
static const char *names[NF] = {"cpu_avg_c","cpu_max_c","gpu_avg_c","gpu_max_c","mem_c","ssd_c","battery_c","fan0_rpm","fan1_rpm","system_w"};
static void sample(float v[NF]) {
  float a, m;
  hid_group("PMU tdie", &v[CPU], &v[CPUMAX]);                // CPU die sensors
  if (v[CPU] < 0) { smc_group("Tp", &v[CPU], &v[CPUMAX]); }   // fallback: SMC CPU cores
  smc_group("Tg", &v[GPU], &v[GPUMAX]);
  smc_group("Tm", &v[MEM], &m);
  hid_group("NAND", &v[SSD], &m);
  hid_group("gas gauge battery", &v[BATT], &m);
  if (v[BATT] < 0) { v[BATT] = smc_get("TB0T"); }
  v[FAN0] = smc_get("F0Ac"); v[FAN1] = smc_get("F1Ac"); v[WATTS] = smc_get("PSTR");
  (void)a;
}

/* ---------- drawing (braille: each cell is 2 samples wide, 4 dots tall) ---------- */
#define HIST 4096
#define NS 5                       /* CPU, GPU, SSD, Battery, Fan (fan has its own chart) */
static float hist[NS][HIST]; static int hn;
static const char *snames[4] = {"CPU", "GPU", "SSD", "Battery"};
static const int sidx[4] = {CPU, GPU, SSD, BATT};
static const char *col[NS] = {"\033[31m", "\033[32m", "\033[36m", "\033[33m", "\033[35m"};
static float lo[NF], hi[NF];
static volatile sig_atomic_t stop;
static void on_int(int s) { (void)s; stop = 1; }

static const unsigned char dotbit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
#define MAXR 200
#define MAXC 1024
static unsigned char cell[MAXR][MAXC]; static signed char ccol[MAXR][MAXC];

static void setdot(int x, int y, int rows, int s) {     /* x,y in dots; y=0 is top */
  if (y < 0) y = 0; if (y >= rows * 4) y = rows * 4 - 1;
  int r = y / 4, c = x / 2;
  cell[r][c] |= dotbit[x & 1][y & 3];
  ccol[r][c] = (signed char)s;                          /* later series wins the cell colour */
}
/* Plot series [first..last) of hist over the last cols*2 samples. Lines connect samples. */
static void chart(int first, int last, float ymin, float ymax, int rows, int cols, int labelEvery, const char *fmt) {
  if (rows > MAXR) rows = MAXR; if (cols > MAXC) cols = MAXC;
  memset(cell, 0, sizeof cell); memset(ccol, -1, sizeof ccol);
  int npts = cols * 2, start = hn > npts ? hn - npts : 0, cnt = hn - start, H = rows * 4;
  for (int s = last - 1; s >= first; s--) {             /* draw CPU last so it stays on top */
    int py = -1;
    for (int i = 0; i < cnt; i++) {
      float t = hist[s][(start + i) % HIST];
      if (t < 0) { py = -1; continue; }
      int y = (int)((ymax - t) / (ymax - ymin) * (H - 1) + 0.5f);
      if (py < 0) setdot(i, y, rows, s);
      else { int a = py < y ? py : y, b = py < y ? y : py; for (int k = a; k <= b; k++) setdot(i, k, rows, s); }
      py = y;
    }
  }
  for (int r = 0; r < rows; r++) {
    float top = ymax - (ymax - ymin) * r / rows;
    if (r % labelEvery == 0) { printf(fmt, top); printf(" ┤"); } else printf("       │");
    for (int c = 0; c < cols; c++) {
      if (!cell[r][c]) { putchar(' '); continue; }
      unsigned b = cell[r][c];
      printf("%s%c%c%c\033[0m", col[(int)ccol[r][c]], 0xE2, 0xA0 | (b >> 6), 0x80 | (b & 0x3F));
    }
    putchar('\n');
  }
  printf(fmt, ymin); printf(" └"); for (int c = 0; c < cols; c++) printf("─"); putchar('\n');
}

static void draw(float v[NF], int interval, const char *logpath) {
  struct winsize w; int W = 80, H = 24;
  if (!ioctl(1, TIOCGWINSZ, &w) && w.ws_col) { W = w.ws_col; H = w.ws_row; }
  int cols = W - 9; if (cols < 10) cols = 10; if (cols > MAXC) cols = MAXC;
  int hasfan = v[FAN0] >= 0;
  int fanrows = hasfan ? 4 : 0;
  int rows = H - 15 - (hasfan ? fanrows + 2 : 0); if (rows < 4) rows = 4;
  int npts = cols * 2, start = hn > npts ? hn - npts : 0, cnt = hn - start;

  float ymin = 1e9, ymax = -1e9, fmax = 0;
  for (int i = start; i < hn; i++) {
    for (int s = 0; s < 4; s++) { float t = hist[s][i % HIST]; if (t < 0) continue;
      if (t < ymin) ymin = t; if (t > ymax) ymax = t; }
    if (hist[4][i % HIST] > fmax) fmax = hist[4][i % HIST];
  }
  ymin = (float)((int)(ymin / 5) * 5); ymax = (float)((int)(ymax / 5) * 5 + 5);
  if (ymax - ymin < 10) ymax = ymin + 10;
  fmax = fmax < 1000 ? 1000 : ((int)(fmax / 1000) + 1) * 1000.0f;

  time_t now = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
  printf("\033[H\033[2J\033[1mthermals\033[0m  %s  every %ds  pressure: %s   (Ctrl-C to quit)\n\n", ts, interval, pressure());
  printf("  %-10s %8s %8s %8s\n", "", "now", "min", "max");
  #define ROW(lbl, i, unit) if (v[i] >= 0) printf("  %-10s %7.1f%s %7.1f%s %7.1f%s\n", lbl, v[i], unit, lo[i], unit, hi[i], unit)
  printf("%s", col[0]); ROW("CPU avg", CPU, "°"); ROW("CPU hot", CPUMAX, "°"); printf("\033[0m");
  printf("%s", col[1]); ROW("GPU avg", GPU, "°"); ROW("GPU hot", GPUMAX, "°"); printf("\033[0m");
  ROW("Memory", MEM, "°");
  printf("%s", col[2]); ROW("SSD", SSD, "°"); printf("\033[0m");
  printf("%s", col[3]); ROW("Battery", BATT, "°"); printf("\033[0m");
  if (hasfan) printf("%s  %-10s %5.0f / %.0f rpm   (max %.0f)\033[0m\n", col[4], "Fans", v[FAN0], v[FAN1] < 0 ? 0 : v[FAN1], hi[FAN0] > hi[FAN1] ? hi[FAN0] : hi[FAN1]);
  if (v[WATTS] >= 0) printf("  %-10s %7.1f W   (max %.1f)\n", "Power", v[WATTS], hi[WATTS]);
  printf("\n");
  chart(0, 4, ymin, ymax, rows, cols, 2, "%5.0f°");
  printf("        ");
  for (int s = 0; s < 4; s++) printf("%s⣿ %s\033[0m  ", col[s], snames[s]);
  printf(" last %ds\n", cnt * interval);
  if (hasfan) {
    printf("\n");
    chart(4, 5, 0, fmax, fanrows, cols, 2, "%6.0f");
    printf("        %s⣿ Fan rpm (faster fan)\033[0m", col[4]);
  }
  if (logpath) printf("   log: %s", logpath);
  printf("\n"); fflush(stdout);
}

static void usage(void) {
  puts("usage: thermals [-i secs] [-o file.csv] [-q] [-1] [-a]\n"
       "  -i secs   sample interval (default 2)\n"
       "  -o file   CSV log path (default ~/Library/Logs/thermals/YYYY-MM-DD.csv)\n"
       "  -q        don't log to CSV\n"
       "  -1        print one reading and exit\n"
       "  -a        dump every raw sensor and exit");
}

int main(int argc, char **argv) {
  int interval = 2, nolog = 0, once = 0, all = 0, c; const char *out = NULL;
  while ((c = getopt(argc, argv, "i:o:q1ah")) != -1) switch (c) {
    case 'i': interval = atoi(optarg); if (interval < 1) interval = 1; break;
    case 'o': out = optarg; break; case 'q': nolog = 1; break;
    case '1': once = 1; break; case 'a': all = 1; break; default: usage(); return c != 'h';
  }
  smc_open(); hid_open();
  if (all) {
    hid_each(print_cb, NULL);
    for (int i = 0; i < nkeys; i++) { char k[5] = {keys[i]>>24, keys[i]>>16, keys[i]>>8, keys[i], 0};
      printf("  SMC %-24s %8.1f\n", k, smc_read_idx(i)); }
    return 0;
  }
  float v[NF];
  if (once) {
    sample(v);
    for (int i = 0; i < NF; i++) if (v[i] >= 0) printf("%-10s %.1f\n", names[i], v[i]);
    printf("pressure   %s\n", pressure());
    return 0;
  }
  char path[1024] = ""; FILE *log = NULL;
  if (!nolog) {
    if (out) snprintf(path, sizeof path, "%s", out);
    else {
      const char *home = getenv("HOME"); char dir[900]; time_t t = time(NULL); char d[16];
      snprintf(dir, sizeof dir, "%s/Library/Logs/thermals", home); mkdir(dir, 0755);
      strftime(d, sizeof d, "%Y-%m-%d", localtime(&t)); snprintf(path, sizeof path, "%s/%s.csv", dir, d);
    }
    struct stat st; int fresh = stat(path, &st) != 0;
    log = fopen(path, "a");
    if (!log) { perror(path); return 1; }
    if (fresh) { fprintf(log, "time"); for (int i = 0; i < NF; i++) fprintf(log, ",%s", names[i]); fprintf(log, ",pressure\n"); }
  }
  for (int i = 0; i < NF; i++) { lo[i] = 1e9; hi[i] = -1e9; }
  signal(SIGINT, on_int); signal(SIGTERM, on_int);
  printf("\033[?25l");                                            // hide cursor
  while (!stop) {
    sample(v);
    for (int i = 0; i < NF; i++) if (v[i] >= 0) { if (v[i] < lo[i]) lo[i] = v[i]; if (v[i] > hi[i]) hi[i] = v[i]; }
    for (int s = 0; s < 4; s++) hist[s][hn % HIST] = v[sidx[s]];
    hist[4][hn % HIST] = v[FAN0] < 0 ? -1 : (v[FAN1] > v[FAN0] ? v[FAN1] : v[FAN0]);
    hn++;
    if (log) {
      time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&t));
      fprintf(log, "%s", ts);
      for (int i = 0; i < NF; i++) v[i] >= 0 ? fprintf(log, ",%.1f", v[i]) : fprintf(log, ",");
      fprintf(log, ",%s\n", pressure()); fflush(log);
    }
    draw(v, interval, log ? path : NULL);
    for (int i = 0; i < interval * 10 && !stop; i++) usleep(100000);
  }
  printf("\033[?25h\n");
  if (log) { fclose(log); printf("saved to %s\n", path); }
  return 0;
}
