# Ultrasight - Using CoreSight tracing on an UltraScale+

> *Note:* This project is a fork of [Ricerca Security's `coresight-trace`](https://github.com/RICSecLab/coresight-trace), a hardware-assisted binary-only fuzzing tool for ARM64 Linux. This project removes the decoding capabilities of the previous one, and provides a port for the UltraScale+ (ZCU-104).


## Context

The **CoreSight technology** is the Arm solution for debug and trace with low overhead in complex SoC designs. CoreSight consists of:
- A library of modular devices and component interconnects.
- Architected discovery and identification methods to allow for flexible system design.
- A standard for implementing the Arm Debug Interface for debug tools.

A corresponding library, [**CSAL**](https://github.com/ARM-software/CSAL) provides helper functions to access configuration registers of components and devices on the board, along with discovery tools.

The CoreSight trace provides a **fine-grained view of the execution path** of a given program, useful for debugging purposes but also to enforce security guarantees or determine code coverage.


## Prerequisites

#### Hardware

This project is based on the **UltraScale+ (ZCU-104)** board that contains both Cortex-A53 and Cortex-R5 cores along with a programmable part. The traced program is expected to run on the Cortex-A53, storing the trace in RAM. Eventually, a trace decoder should be able on the board through the FPGA part, directly accessing the trace buffer.

#### Environment

The project is expected to run on an ARM64 Linux, and requires access to physical memories to operate on CoreSight components. It is built alongside a custom version of [CSAL](https://github.com/QDucasse/csal) to provide support for the UltraScale+ (configuration, additional TMC configurations, *etc*).

The storage of the trace buffer in memory is performed using the [`u-dma-buf`](https://github.com/ikwzm/u-dma-buf) (user-mappable DMA buffer) kernel module. It allocates a DMA-capable continuous memory region, and the tracer uses this region to store trace data.

> *Note:* The setup of this module on its own is presented in the "build" part but it can also be embedded in a BitBake recipe and in the device tree. This way, it can be added to reserved memory, guaranteeing no other accesses will be performed to this region.


## Getting started


#### `udmabuf` setup

The main trace buffer is stored in an ETR, stored in RAM. This is done using the open-source [`u-dma-buf`](https://github.com/ikwzm/u-dma-buf) project and kernel module. To install it directly:

```bash
$ git clone https://github.com/ikwzm/u-dma-buf
$ cd u-dma-buf
$ make
$ sudo insmod u-dma-buf.ko udmabuf0=0x80000
```

This creates a `/dev/udmabuf0` buffer of size `0x80000`. You can find more information looking for the DMA region in `/proc/iomem` and its size/physical address directly:

```bash
$ cat /proc/iomem
$ cat /sys/class/u-dma-buf/udmabuf0/phys_addr
$ cat /sys/class/u-dma-buf/udmabuf0/size
```

#### Build

Initialize the repository:

```bash
$ git clone https://github.com/QDucasse/ultrasight
$ cd ultrasight
$ git submodule update --init
```

Then, get the correct headers for the fuzzsight Vivado project:

```bash
$ make bd-addrs-apply FUZZSIGHT_DIR=/path/to/fuzzsight

  MACRO                    BLOCK DESIGN   HEADER         STATUS
  DMA_BASE                 0x80000000     0x80000000     ok
  BITMAP_READER_BASE       0x80010000     0x80000000     ok
  DECODER_STATS_BASE_ETM   0x80020000     0x80020000     ok
  EDGE_EXTRACTOR_BASE      0x80021000     0x80021000     ok
  DECODER_AXI_BASE         0x80022000     0x80022000     ok
```

> **Note:** `bd-addrs` simply displays them and `sync-headers` copy any changes in the project to here

You can then build with:

```bash
$ make trace // runs cs-trace on tests/fib (described in next section)
```


## Standalone tracing with `cs-trace`

`cs-trace` is the standalone tracer. It forks the target under `ptrace`, configures the
CoreSight components (ETMs, funnels, ETFs, ETR, TPIU), releases the child, and on exit
reads the trace buffer out of the ETR into `cstrace.bin`.

```bash
$ sudo ./cs-trace [OPTIONS] -- EXE [ARGS]
```

Root is required: the tool maps CoreSight configuration registers through `/dev/mem`.

#### Options

| Option | Default | Description |
| --- | --- | --- |
| `-b, --board=NAME` | `ZCU-104` | Board profile from `include/known_boards.h`. |
| `-c, --cpu=INT` | `-1` | Pin the traced process to a CPU. `-1` picks one automatically (preferred CPU of the parent, else a free one). |
| `-e, --export` | off | Write an OpenCSD snapshot (`snapshot.ini`, `device_*.ini`, `cstrace.bin`) into the current directory. Required for `trc_pkt_lister`. |
| `-f, --fetcher` | off | Run the background fetcher thread, draining the buffer during execution instead of once at exit. |
| `-u, --udmabuf=INT` | `0` | `u-dma-buf` device number backing the ETR. |
| `-k, --ksight` | off | Collect ksight kernel tag events. |
| `-r, --useetr=0\|1` | `1` | Use the ETR (trace to SDRAM). With `0` there is no main sink and nothing is captured for software decoding. |
| `-s, --usestm=0\|1` | `1` | Enable the STM/ITM software-stimulus source (trace ID `0x20`). |
| `-t, --teardownetf=0\|1` | `1` | Disable the ETFs on teardown rather than leaving them running. |
| `-m, --singlecpu=0\|1` | `0` | Enable/disable only the traced CPU's ETM instead of all four. Cuts trace volume roughly in half by removing the other cores' streams. |
| `-a, --branchbroadcast=0\|1` | `1` | Enable ETM branch broadcasting. |
| `-v, --verbose[=INT]` | `0` | Verbosity. `>0` dumps the cross-trigger config, `>1` the ETM config. |
| `-n, --no-trace` | off | Baseline mode: `ptrace` attach/detach only, no CoreSight setup. Useful for measuring the tool's own overhead. |
| `-o, --csv=PATH` | disabled | Append one row of decoder / edge / decoder-AXI stats to a CSV. |
| `-g, --hashmode=MODE` | `stalker` | Edge hash mode: `none`, `fuzzsight` or `stalker`. Only decides which index an edge maps to; edge counts are identical across modes. |

Boolean options take an explicit argument (`--singlecpu=1`), except `--export`, `--fetcher`,
`--ksight` and `--no-trace`, which are flags.


#### Trace decoding using [OpenCSD](https://github.com/Linaro/OpenCSD)

The initial authors of `coresight-trace` used an in-house decoder tied to their needs (doing binary-only fuzzing, and assessing code coverage of the fuzzed binary). OpenCSD is a fully-fledged decoder developped by Linaro, handling all versions of ETM along with STM support. It is meant to be used as a library linked against our application but still provide a test utility that can list CoreSight trace packets. The utility `trc_pkt_lister` can be used on a snapshot as follows:

```bash
# Clone and build the library/programs
$ git clone https://github.com/Linaro/OpenCSD
$ cd OpenCSD/decoder/build/linux
$ make
```

In the `OpenCSD/decoder/tests/build/builddir/` directory lies the `trc_pkt_lister` program!


The snapshot structure expected by `trc_pkt_lister` is produced by `cs-trace`'s `--export` option, which `make trace` passes by default. `make trace` also invokes the decoder for you. To run it by hand against an existing snapshot directory:

```bash
$ cd ultrasight
$ make trace
$ trc_pkt_lister -ss_dir trace/<datetime>/ -stats
...
Idx:6419; ID:13;        I_TRACE_INFO : Trace Info.; INFO=0x0 { CC.0 }
Idx:6422; ID:13;        I_ADDR_L_64IS0 : Address, Long, 64 bit, IS0.; Addr=0xFFFF800008E1A7C0;
Idx:6432; ID:11;        I_ASYNC : Alignment Synchronisation.
Idx:6444; ID:11;        I_TRACE_INFO : Trace Info.; INFO=0x0 { CC.0 }
Idx:6449; ID:11;        I_ADDR_L_64IS0 : Address, Long, 64 bit, IS0.; Addr=0xFFFF800008E1A7C0;
Idx:6458; ID:10;        I_TRACE_ON : Trace On.
Idx:6459; ID:10;        I_ADDR_CTXT_L_64IS0 : Address & Context, Long, 64 bit, IS0.; Addr=0x0000AAAAC7260740; Ctxt: AArch64,EL0, NS; CID=0x0000042d;
Idx:6475; ID:10;        I_ATOM_F2 : Atom format 2.; EE
Idx:6476; ID:10;        I_ADDR_L_64IS0 : Address, Long, 64 bit, IS0.; Addr=0x0000AAAAC7260670;
Idx:6486; ID:10;        I_ATOM_F1 : Atom format 1.; E
Idx:6487; ID:10;        I_ADDR_L_64IS0 : Address, Long, 64 bit, IS0.; Addr=0x0000FFFFB737F9D4;
Idx:6497; ID:10;        I_TRACE_ON : Trace On.
Idx:6498; ID:10;        I_ADDR_CTXT_L_64IS0 : Address & Context, Long, 64 bit, IS0.; Addr=0x0000AAAAC7260650; Ctxt: AArch64,EL0, NS; CID=0x0000042d;
Idx:6513; ID:10;        I_ATOM_F3 : Atom format 3.; EEE
Idx:6514; ID:10;        I_ADDR_L_64IS0 : Address, Long, 64 bit, IS0.; Addr=0x0000AAAAC7260660;
Idx:6523; ID:10;        I_ATOM_F1 : Atom format 1.; E
Idx:6524; ID:10;        I_ADDR_L_64IS0 : Address, Long, 64 bit, IS0.; Addr=0x0000FFFFB6B478BC;
...
ID:10   END OF TRACE DATA
ID:11   END OF TRACE DATA
ID:12   END OF TRACE DATA
ID:13   END OF TRACE DATA
Trace Packet Lister : Trace buffer done, processed 6912 bytes in 4.7432062 seconds.
```


## License

Ricerca Security released `coresight-trace` under the [Apache License, Version 2.0](https://opensource.org/licenses/Apache-2.0). Changes to the initial project are documented in each modified file.
