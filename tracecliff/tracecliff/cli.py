"""
tracecliff cli — single entry point aggregating every tracecliff subcommand.

    python3 -m tracecliff <subcommand> [args]

Subcommands:
    sweep-addr       drive cs-trace over bench_addr.c binaries
    run-all          run sweep-addr, sharing cs-trace/cs-flags/runs/etr-list/bb-list

sweep-addr and run-all must be run as root (cs-trace needs /dev/mem access), e.g.:

    sudo python3 -m tracecliff run-all --cs-trace /path/to/cs-trace --cs-flags "-b ZCU-104 -c 0"
"""

import argparse
import shlex
from pathlib import Path

from tracecliff import sweep

REPO_ROOT = Path(__file__).resolve().parent.parent


def add_run_all_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--cs-trace", required=True, help="path to cs-trace binary")
    parser.add_argument(
        "--cs-flags",
        default="",
        help='fixed flags passed to cs-trace for every run, e.g. "-b ZCU-104 -c 0"',
    )
    parser.add_argument(
        "--runs", type=int, default=3, help="repetitions per point (default: 3)"
    )
    parser.add_argument(
        "--etr-list",
        nargs="+",
        type=int,
        default=[0, 1],
        help="--useetr values to sweep (default: 0 1)",
    )
    parser.add_argument(
        "--bb-list",
        nargs="+",
        type=int,
        default=[0, 1],
        help="--branchbroadcast values to sweep (default: 0 1)",
    )


def run_all(args: argparse.Namespace) -> None:
    cs_flags = shlex.split(args.cs_flags)

    addr_cfg = sweep.Config(
        cs_trace=args.cs_trace,
        bench_dir=REPO_ROOT / sweep.DEFAULT_BENCH_SUBDIR,
        cs_flags=cs_flags,
        runs=args.runs,
        etr_list=args.etr_list,
        bb_list=args.bb_list,
    )
    print("=== addr_loop ===")
    sweep.run(addr_cfg)


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
        '    sudo python3 -m tracecliff sweep-addr --cs-trace /path/to/cs-trace --cs-flags "-b ZCU-104 -c 0"',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sweep.add_arguments(p)
    p.set_defaults(func=lambda args: sweep.run(sweep.config_from_args(args)))

    p = subparsers.add_parser(
        "run-all",
        help="run sweep-addr",
        description="Run sweep-addr, "
        "sharing cs-trace/cs-flags/runs/etr-list/bb-list. Must be run as root.\n\n"
        "Example:\n"
        "    sudo python3 -m tracecliff run-all --cs-trace /path/to/cs-trace",
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
