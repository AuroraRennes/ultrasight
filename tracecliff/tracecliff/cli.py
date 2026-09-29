"""
tracecliff cli — single entry point aggregating every tracecliff subcommand.

    python3 -m tracecliff <subcommand> [args]

Subcommands:
    sweep-addr       drive cs-trace over bench_addr.c binaries
    sweep-call       drive cs-trace over bench_call.c binaries (range-crossing stress)
    gen-chain        emit chain-mode stub assembly for bench_addr.c's chain mode
    analyze          read sweep CSVs back and report loss%
    micro            report each sweep point as a difference from the reference point
    axes             print the sweep axes (--make: as Make variables, for axes.mk)
    run-all          run sweep-addr (kind=loop, kind=chain) then sweep-call
                     sequentially, sharing cs-trace/cs-flags/runs and the
                     swept factors

sweep-addr, sweep-call and run-all must be run as root (cs-trace needs /dev/mem access), e.g.:

    sudo python3 -m tracecliff run-all --cs-flags "-b ZCU-104 -c 0"

cs-trace defaults to ../cs-trace, the ultrasight build tracecliff lives in.
Pass --cs-trace only for an out-of-tree build.
"""

import argparse
import shlex
from pathlib import Path

from tracecliff import analyzer, benches, gen_chain, sweep

REPO_ROOT = Path(__file__).resolve().parent.parent


def add_addr_arguments(parser: argparse.ArgumentParser) -> None:
    sweep.add_common_arguments(
        parser,
        bench_dir_help="directory containing bench_n*_s*_i* binaries "
        "(default: bin/addr_loop or bin/addr_chain, from --kind)",
    )
    parser.add_argument(
        "--kind",
        choices=["loop", "chain"],
        default="loop",
        help="loop (bench_*) | chain (bench_chain_*) (default: loop)",
    )


def addr_config(args: argparse.Namespace) -> sweep.Config:
    return sweep.config_from_args(
        args, benches.addr_bench(args.kind), benches.ADDR_BENCH_SUBDIR[args.kind]
    )


def add_call_arguments(parser: argparse.ArgumentParser) -> None:
    sweep.add_common_arguments(
        parser,
        bench_dir_help="directory containing bench_call_k*_l*_i* binaries "
        "(default: bin/call, or bin/call_fill with --fill)",
    )
    parser.add_argument(
        "--fill",
        action="store_true",
        help="sweep the call_fill family (bench_call_fill_k*_i*): CALL_LEN=1 "
        "with a branch-free filler per iteration",
    )


def call_config(args: argparse.Namespace) -> sweep.Config:
    if getattr(args, "fill", False):
        return sweep.config_from_args(
            args, benches.CALL_FILL_BENCH, benches.CALL_FILL_BENCH_SUBDIR
        )
    return sweep.config_from_args(args, benches.CALL_BENCH, benches.CALL_BENCH_SUBDIR)


def add_run_all_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--cs-trace",
        default=sweep.DEFAULT_CS_TRACE,
        type=Path,
        help=f"path to cs-trace binary (default: {sweep.DEFAULT_CS_TRACE})",
    )
    parser.add_argument(
        "--cs-flags",
        default="",
        help='fixed flags passed to cs-trace for every run, e.g. "-b ZCU-104 -c 0"',
    )
    parser.add_argument(
        "--runs", type=int, default=3, help="repetitions per point (default: 3)"
    )
    sweep.add_factor_arguments(parser)


def run_all(args: argparse.Namespace) -> None:
    cs_flags = shlex.split(args.cs_flags)
    factors = sweep.factors_from_args(args)

    runs = [
        (benches.addr_bench(kind), benches.ADDR_BENCH_SUBDIR[kind])
        for kind in ("loop", "chain")
    ] + [(benches.CALL_BENCH, benches.CALL_BENCH_SUBDIR)]

    for bench, subdir in runs:
        cfg = sweep.Config(
            bench=bench,
            cs_trace=args.cs_trace,
            bench_dir=REPO_ROOT / subdir,
            cs_flags=cs_flags,
            runs=args.runs,
            factors=factors,
        )
        print(f"=== {bench.name} ===")
        sweep.run(cfg)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tracecliff",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    p = subparsers.add_parser(
        "sweep-addr",
        help="drive cs-trace over bench_addr.c binaries",
        description="Drive cs-trace over bench_addr.c binaries (indirect branch / "
        "address-packet stress). Must be run as root.\n\n"
        "Example:\n"
        "    sudo python3 -m tracecliff sweep-addr --kind chain --factor stall=off,3",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_addr_arguments(p)
    p.set_defaults(func=lambda args: sweep.run(addr_config(args)))

    p = subparsers.add_parser(
        "sweep-call",
        help="drive cs-trace over bench_call.c binaries",
        description="Drive cs-trace over bench_call.c binaries (PLT calls out of the "
        "traced range into libc). Must be run as root.\n\n"
        "Example:\n"
        "    sudo python3 -m tracecliff sweep-call --factor addrfilter=etm,none",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_call_arguments(p)
    p.set_defaults(func=lambda args: sweep.run(call_config(args)))

    p = subparsers.add_parser(
        "gen-chain",
        help="emit chain-mode stub assembly",
        description="Emit the hand-written chain-mode stub assembly used by bench_addr.c's chain mode.\n\n"
        "Example:\n"
        "    python3 -m tracecliff gen-chain 8 64 > chains/chain_n8_s64.S",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    gen_chain.add_arguments(p)
    p.set_defaults(func=lambda args: gen_chain.run(args.n_targets, args.stub_stride))

    p = subparsers.add_parser(
        "analyze",
        help="read sweep CSVs back and report loss%%",
        description="Read sweep CSVs back and report loss% and delivered edge rate.\n\n"
        "Example:\n"
        "    python3 -m tracecliff analyze --csv-out",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    analyzer.add_arguments(p)
    p.set_defaults(func=analyzer.run)

    p = subparsers.add_parser(
        "micro",
        help="report each sweep point as a difference from the reference point",
        description="Report each sweep point as a difference from the reference point.\n\n"
        "Unlike analyze, nothing is measured against an analytic baseline: the\n"
        "sweeps vary cs-trace configuration, so the question is what one knob did.\n\n"
        "Example:\n"
        "    python3 -m tracecliff micro --per-branch",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    analyzer.add_arm_arguments(p)
    p.set_defaults(func=analyzer.run_arms)

    p = subparsers.add_parser(
        "axes",
        help="print the sweep axes (--make: as Make variables)",
        description="Print the sweep axes declared in tracecliff/benches.py.\n\n"
        "--make emits them as Make variables; the Makefile includes the result\n"
        "(axes.mk) instead of repeating the lists, so the binaries it builds and\n"
        "the grid the sweeps walk cannot drift apart.\n\n"
        "Example:\n"
        "    python3 -m tracecliff axes --make > axes.mk",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    benches.add_arguments(p)
    p.set_defaults(func=benches.run)

    p = subparsers.add_parser(
        "run-all",
        help="run sweep-addr (loop, chain) then sweep-call sequentially",
        description="Run sweep-addr (kind=loop, kind=chain) then sweep-call sequentially, "
        "sharing cs-trace/cs-flags/runs and the swept factors. Must be run as root.\n\n"
        "Example:\n"
        "    sudo python3 -m tracecliff run-all",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    add_run_all_arguments(p)
    p.set_defaults(func=run_all)

    return parser


def main(argv: list[str] | None = None) -> None:
    parser = build_parser()
    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
