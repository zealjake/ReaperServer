#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path


REQUIRED_COLUMNS = ("Comment", "Timecode")
OUTPUT_COLUMNS = ("#", "Name", "Start", "End", "Length")


def read_frameio_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError("Input CSV is missing a header row")

        missing = [column for column in REQUIRED_COLUMNS if column not in reader.fieldnames]
        if missing:
            raise ValueError(f"Input CSV is missing required columns: {', '.join(missing)}")

        return list(reader)


def convert_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    converted: list[dict[str, str]] = []

    for index, row in enumerate(rows, start=1):
        comment = (row.get("Comment") or "").strip()
        timecode = (row.get("Timecode") or "").strip()
        if not comment or not timecode:
            continue

        converted.append(
            {
                "#": f"M{index}",
                "Name": comment,
                "Start": timecode,
                "End": "",
                "Length": "",
            }
        )

    return converted


def write_reaper_rows(output_path: Path, rows: list[dict[str, str]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=OUTPUT_COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert a Frame.io comments CSV into a REAPER marker CSV."
    )
    parser.add_argument("input_csv", type=Path, help="Path to the Frame.io CSV export")
    parser.add_argument("output_csv", type=Path, help="Path to write the REAPER marker CSV")
    return parser


def main() -> int:
    parser = build_argument_parser()
    args = parser.parse_args()

    rows = read_frameio_rows(args.input_csv)
    converted = convert_rows(rows)
    write_reaper_rows(args.output_csv, converted)

    print(f"Wrote {len(converted)} markers to {args.output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
