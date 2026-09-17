# Stable text rendering, shared by the command line and the local web UI.
#
# The shapes here are the ones a session reads: one line per row in lists,
# "<label> <value>" in details, times in UTC. Nothing here ever prints a
# token, a private key, an install id or a log line.
import datetime

DATABASE_LIMIT_BYTES = 500 * 1000 * 1000        # D1 Free refuses writes there
DATABASE_WARN_BYTES = 450 * 1000 * 1000

KIND_TITLES = {
    "halt": "Halt",
    "guest_fault": "Guest fault",
    "host_fault": "Native fault",
    "abnormal_exit": "Abnormal exit",
    "hang": "Hang",
}


def utc(unix, with_seconds=True):
    """Unix seconds as UTC text, the way every list and detail shows them."""
    if unix is None:
        return "-"
    moment = datetime.datetime.fromtimestamp(int(unix), datetime.timezone.utc)
    return moment.strftime("%Y-%m-%d %H:%M:%S UTC" if with_seconds else "%Y-%m-%d %H:%M")


def human_bytes(count):
    if count is None:
        return "-"
    size = float(count)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} GiB"


def ellipsis(text, width):
    text = text or ""
    return text if len(text) <= width else text[:width - 3] + "..."


def table(headers, rows):
    """Aligned columns; every cell is already a string."""
    widths = [len(h) for h in headers]
    for row in rows:
        for index, cell in enumerate(row):
            widths[index] = max(widths[index], len(cell))
    lines = ["  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)).rstrip()]
    for row in rows:
        lines.append("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)).rstrip())
    return lines


def labelled(pairs, width=12):
    return [f"{label.ljust(width)} {value}" for label, value in pairs]


# ----------------------------------------------------------------- signatures

SIGNATURE_HEADERS = ("SIGNATURE", "KIND", "STATUS", "COUNT", "CONSOLES", "LAST SEEN", "SAMPLE", "ISSUE", "CANON")


def signature_row(item):
    return (
        item.id,
        item.kind,
        item.status,
        str(item.count),
        str(item.installs),
        utc(item.last_seen_unix, with_seconds=False),
        item.sample_state,
        "yes" if item.issue_url else "-",
        ellipsis(item.canon, 60),
    )


def signature_list(items):
    if not items:
        return ["no signature"]
    return table(SIGNATURE_HEADERS, [signature_row(item) for item in items])


def signature_detail(detail):
    lines = labelled([
        ("signature", detail.id),
        ("kind", detail.kind),
        ("status", detail.status + (f" (fixed in {detail.fixed_in_version})" if detail.fixed_in_version else "")),
        ("canon", detail.canon),
        ("count", f"{detail.count} claims from {detail.installs} console(s)"),
        ("first seen", utc(detail.first_seen_unix)),
        ("last seen", utc(detail.last_seen_unix)),
        ("versions", ", ".join(detail.versions()) or "-"),
        ("sample", sample_text(detail)),
        ("issue", detail.issue_url or "-"),
        ("merged into", detail.merged_into or "-"),
        ("note", detail.note or "-"),
        ("rules", f"v{detail.rules_version}"),
    ])
    if detail.builds:
        lines += ["", "builds"]
        lines += ["  " + line for line in table(
            ("BUILD", "COUNT", "FIRST SEEN", "LAST SEEN"),
            [(b.build_id, str(b.count), utc(b.first_seen_unix, False), utc(b.last_seen_unix, False))
             for b in detail.builds])]
    if detail.sample_artifacts:
        lines += ["", "sample artifacts"]
        lines += ["  " + line for line in table(
            ("NAME", "SEALED BYTES", "STORED", "SHA256"),
            [(a.name, str(a.bytes), utc(a.stored_unix, False), a.sha256[:16] + "...")
             for a in detail.sample_artifacts])]
    if detail.recent_reports:
        lines += ["", "recent reports"]
        lines += ["  " + line for line in table(
            ("REPORT", "BUILD", "RECEIVED", "ACTION"),
            [(r.report_id, r.build_id, utc(r.received_unix, False), r.action) for r in detail.recent_reports])]
    return lines


def sample_text(detail):
    if detail.sample_state == "stored" and detail.sample_report:
        return f"stored, report {detail.sample_report}"
    if detail.sample_state == "leased":
        return f"leased by {detail.lease_report or '?'} until {utc(detail.lease_expires_unix)}"
    return detail.sample_state


# ----------------------------------------------------------------------- bugs

BUG_HEADERS = ("BUG", "STATUS", "CREATED", "VERSION", "LANG", "ISSUE", "TITLE")


def bug_row(bug):
    return (bug.id, bug.status, utc(bug.created_unix, False), ellipsis(bug.version, 20), bug.lang,
            "yes" if bug.issue_url else "-", ellipsis(bug.title, 60))


def bug_list(items):
    if not items:
        return ["no bug report"]
    return table(BUG_HEADERS, [bug_row(bug) for bug in items])


def bug_detail(bug):
    lines = labelled([
        ("bug", bug.id),
        ("status", bug.status),
        ("created", utc(bug.created_unix)),
        ("version", bug.version),
        ("language", bug.lang),
        ("contact", bug.contact or "-"),
        ("issue", bug.issue_url or "-"),
        ("title", bug.title),
    ])
    lines += ["", "description"]
    lines += ["  " + line for line in (bug.description or "").splitlines() or ["-"]]
    return lines


# ---------------------------------------------------------------------- stats

def usage_line(usage):
    share = f"{100 * usage.share:.0f}%" if usage.cap else "-"
    return f"{usage.used} / {usage.cap} ({share})"


def stats_lines(stats):
    """(lines, warnings): the quota page, plus what needs attention."""
    warnings = []
    lines = labelled([
        ("day", stats.day + " (UTC)"),
        ("accepting", "yes" if stats.accepting else "NO (kill switch)"),
        ("claims", usage_line(stats.usage["claims"])),
        ("artifacts", f"{human_bytes(stats.usage['artifact_bytes'].used)} / "
                      f"{human_bytes(stats.usage['artifact_bytes'].cap)}"),
        ("signatures", usage_line(stats.usage["new_signatures"])),
        ("bugs", usage_line(stats.usage["bugs"])),
    ], width=11)
    if stats.database_bytes is not None:
        lines.append(f"{'database'.ljust(11)} {human_bytes(stats.database_bytes)}")
        if stats.database_bytes > DATABASE_WARN_BYTES:
            warnings.append(f"database at {human_bytes(stats.database_bytes)}: D1 Free refuses every write at "
                            f"500 MB. Lower global_claims_per_day, or move to D1 paid "
                            f"(api/README.md, database size).")
    if not stats.accepting:
        warnings.append("the kill switch is on: consoles are answered 503 not_accepting.")
    for name, usage in stats.usage.items():
        if usage.cap and usage.share >= 0.9:
            warnings.append(f"today's {name} quota is at {100 * usage.share:.0f}% of its cap.")
    return lines, warnings
