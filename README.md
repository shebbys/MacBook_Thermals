# MacBook_Thermals

`thermals` is a small command-line program for Apple Silicon Macs. It shows your Mac's
temperatures, fan speeds and power draw as a live graph in the terminal, and saves every
reading to a CSV file so you can look back at how hot the machine got over time. It is
handy for watching long, heavy jobs like local AI inference, builds or video renders.

It reads the hardware sensors directly through macOS's IOKit library, so it needs no
`sudo`, no Homebrew packages and no other dependencies. Building it only needs `clang`,
which comes with Xcode or the Xcode Command Line Tools.

## What it shows

- **CPU temperature:** the average and hottest of the CPU die sensors
- **GPU temperature:** the average and hottest of the GPU sensors
- **Memory temperature**
- **SSD temperature**
- **Battery temperature**
- **Fan speeds** for each fan, in rpm
- **Total system power** in watts
- **Thermal pressure:** macOS's own rating of how hard it is throttling to stay cool
  (nominal, moderate, heavy, trapping or sleeping)

For each value you see the current reading, plus the lowest and highest since the program
started. Below that, two graphs scroll from left to right and stretch to fill your
terminal window:

- A temperature graph with CPU in red, GPU in green, SSD in cyan and battery in yellow.
  It uses braille dots, so each character holds two readings across and four levels up and
  down, which gives smooth lines.
- A fan graph in magenta that shows the faster fan's rpm on its own scale.

## Build

```sh
make            # builds ./thermals
make install    # optional: links it into ~/bin so you can type `thermals` anywhere
make clean      # removes the binary
```

## Usage

```sh
./thermals              # live graph, one sample every 2 seconds, logging to CSV
./thermals -i 5         # sample every 5 seconds
./thermals -q           # live graph, no CSV log
./thermals -o run.csv   # log to a file you choose
./thermals -1           # print one reading and exit (good for scripts)
./thermals -a           # list every raw sensor the Mac exposes and exit
./thermals -h           # help
```

Press **Ctrl-C** to quit. The program puts the cursor back and prints where the log
was saved.

## The CSV log

By default each day gets its own file at `~/Library/Logs/thermals/YYYY-MM-DD.csv`. New
runs on the same day add to the end of that day's file. Each row looks like this:

```
time,cpu_avg_c,cpu_max_c,gpu_avg_c,gpu_max_c,mem_c,ssd_c,battery_c,fan0_rpm,fan1_rpm,system_w,pressure
2026-11-18T17:55:38,36.8,38.0,38.3,38.5,38.7,31.0,31.1,0.0,0.0,8.8,nominal
```

Temperatures are in °C. A blank cell means that sensor isn't present on your Mac. The
file opens directly in Numbers, Excel or any plotting tool.

## How it works

- **SMC (System Management Controller):** the program opens the `AppleSMC` IOKit service,
  lists all of its keys, and reads the floating-point temperature, fan and power values.
  Keys that start with `Tg` are GPU sensors and keys that start with `Tm` are memory
  sensors. `F0Ac` and `F1Ac` are the fan speeds and `PSTR` is total system power.
- **IOKit HID sensor service:** the CPU die sensors (`PMU tdie*`), the SSD (`NAND`) and
  the battery (`gas gauge battery`) are read through the same private HID event system
  that macOS uses internally. If the CPU die sensors aren't available, the program falls
  back to the SMC `Tp*` keys.
- **Thermal pressure** comes from the `com.apple.system.thermalpressurelevel` notification
  state.

Readings below 5 °C or above 130 °C are ignored, because they come from unused or bogus
sensors.

## Notes

- It's built and tested on an M5 Max running macOS 27. Sensor names differ between chip
  generations, so other Apple Silicon Macs should work, but some rows may be missing.
  Run `./thermals -a` to see what your Mac exposes.
- It uses private IOKit functions, so a future macOS update could break it.
- It won't work inside Docker or a VM, because they can't see the Mac's sensors.
- `smcdump.c` is a small helper that prints every SMC temperature, fan and power key with
  its value. It's useful for figuring out which key is which.

## Files

| File | What it is |
|---|---|
| `thermals.c` | The program |
| `Makefile` | Build, install and clean |
| `smcdump.c` | A helper that dumps the SMC keys |
