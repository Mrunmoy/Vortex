#!/usr/bin/env python3
"""Generate GitHub Pages content from CI artifacts."""

import html
import json
import os
import shutil

ARTIFACT_DIR = os.environ.get("ARTIFACT_DIR", "artifacts")
OUT_DIR = os.environ.get("OUT_DIR", "public")


def read_file(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read().strip()
    except FileNotFoundError:
        return None


def copy_coverage():
    src = os.path.join(ARTIFACT_DIR, "coverage", "coverage_html")
    dst = os.path.join(OUT_DIR, "coverage")
    if os.path.isdir(src):
        shutil.copytree(src, dst)
        return True
    return False


def get_coverage_pct():
    pct = read_file(os.path.join(ARTIFACT_DIR, "coverage", "coverage_pct.txt"))
    if pct:
        try:
            return float(pct)
        except ValueError:
            pass
    return None


def coverage_color(pct):
    if pct is None:
        return "lightgrey"
    if pct >= 90:
        return "brightgreen"
    if pct >= 75:
        return "green"
    if pct >= 60:
        return "yellow"
    return "red"


def write_badge_json(pct):
    badge_dir = os.path.join(OUT_DIR, "badges")
    os.makedirs(badge_dir, exist_ok=True)
    label = f"{pct:.1f}%" if pct is not None else "N/A"
    badge = {
        "schemaVersion": 1,
        "label": "coverage",
        "message": label,
        "color": coverage_color(pct),
    }
    with open(os.path.join(badge_dir, "coverage.json"), "w") as f:
        json.dump(badge, f)


def load_benchmarks():
    """Load Google Benchmark JSON results and organize by category."""
    path = os.path.join(ARTIFACT_DIR, "benchmark", "bench_results.json")
    raw = read_file(path)
    if not raw:
        return None
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return None

    benchmarks = data.get("benchmarks", [])
    if not benchmarks:
        return None

    # Group by category (PostLatency, PostThroughput, TimerJitter, FdReaction)
    categories = {
        "PostLatency": {"label": "Post-to-Execute Latency", "unit": "ns", "lower_better": True},
        "PostThroughput": {"label": "Post Throughput", "unit": "ops/s", "lower_better": False},
        "TimerJitter": {"label": "Timer Jitter", "unit": "ns", "lower_better": True},
        "FdReaction": {"label": "FD Reaction Time", "unit": "ns", "lower_better": True},
    }

    results = {}
    for bm in benchmarks:
        name = bm.get("name", "")
        # Parse: BM_{Library}_{Category}/...
        parts = name.split("/")[0]  # Strip /iterations:N etc.
        tokens = parts.split("_")
        if len(tokens) < 3 or tokens[0] != "BM":
            continue
        library = tokens[1]
        category = "_".join(tokens[2:])

        if category not in categories:
            continue

        if category not in results:
            results[category] = {}

        if category == "PostThroughput":
            # items_per_second may be top-level or in counters
            ips = bm.get("items_per_second", 0)
            if ips == 0:
                ips = bm.get("counters", {}).get("items_per_second", 0)
            results[category][library] = ips
        else:
            # Use real_time (manual time in ns)
            results[category][library] = bm.get("real_time", 0)

    return {"categories": categories, "results": results}


def format_value(val, category):
    """Format a benchmark value for display."""
    if category == "PostThroughput":
        if val >= 1e6:
            return f"{val / 1e6:.2f}M"
        if val >= 1e3:
            return f"{val / 1e3:.1f}K"
        return f"{val:.0f}"
    else:
        if val >= 1e6:
            return f"{val / 1e6:.2f} ms"
        if val >= 1e3:
            return f"{val / 1e3:.1f} µs"
        return f"{val:.0f} ns"


def bench_section(bench_data):
    """Generate HTML for the benchmark comparison section."""
    if not bench_data:
        return ""

    categories = bench_data["categories"]
    results = bench_data["results"]

    tables_html = ""
    for cat_key, cat_info in categories.items():
        if cat_key not in results:
            continue

        entries = results[cat_key]
        lower_better = cat_info["lower_better"]

        # Sort: best first
        sorted_entries = sorted(
            entries.items(),
            key=lambda x: x[1],
            reverse=not lower_better,
        )

        best_val = sorted_entries[0][1] if sorted_entries else 0

        rows = ""
        for i, (lib, val) in enumerate(sorted_entries):
            lib_esc = html.escape(lib)
            val_str = html.escape(format_value(val, cat_key))
            badge = "🥇" if i == 0 else ("🥈" if i == 1 else ("🥉" if i == 2 else ""))
            highlight = ' class="winner"' if i == 0 else ""
            rows += f"        <tr{highlight}><td>{badge} {lib_esc}</td><td>{val_str}</td></tr>\n"

        label = html.escape(cat_info["label"])
        hint = "lower is better" if lower_better else "higher is better"
        tables_html += f"""
    <div class="card">
      <h2>{label}</h2>
      <p class="hint">({hint})</p>
      <table>
        <tr><th>Library</th><th>Result</th></tr>
{rows}      </table>
    </div>"""

    return tables_html


def generate_index(has_coverage, coverage_pct, bench_data):
    cov_section = ""
    if has_coverage:
        pct_str = html.escape(f"{coverage_pct:.1f}%" if coverage_pct else "N/A")
        cov_section = f"""
    <div class="card">
      <h2>Code Coverage</h2>
      <p class="metric">{pct_str}</p>
      <a href="coverage/">Full Report &rarr;</a>
    </div>"""

    bench_html = bench_section(bench_data)

    index_html = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Vortex Dashboard</title>
  <style>
    :root {{ --bg: #0d1117; --fg: #c9d1d9; --card: #161b22; --accent: #58a6ff;
             --win: #238636; --table-border: #30363d; }}
    * {{ margin: 0; padding: 0; box-sizing: border-box; }}
    body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, sans-serif;
           background: var(--bg); color: var(--fg); padding: 2rem; }}
    h1 {{ color: var(--accent); margin-bottom: 0.5rem; }}
    h1 + p {{ color: #8b949e; margin-bottom: 1.5rem; }}
    .grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 1.5rem; }}
    .card {{ background: var(--card); border-radius: 8px; padding: 1.5rem; }}
    .card h2 {{ color: var(--accent); margin-bottom: 0.5rem; font-size: 1.1rem; }}
    .card a {{ color: var(--accent); text-decoration: none; }}
    .card a:hover {{ text-decoration: underline; }}
    .metric {{ font-size: 2rem; font-weight: bold; margin: 0.5rem 0; }}
    .hint {{ color: #8b949e; font-size: 0.85rem; margin-bottom: 0.75rem; }}
    table {{ width: 100%; border-collapse: collapse; margin-top: 0.5rem; }}
    th, td {{ padding: 0.5rem 0.75rem; text-align: left; border-bottom: 1px solid var(--table-border); }}
    th {{ color: #8b949e; font-weight: 600; font-size: 0.85rem; text-transform: uppercase; }}
    tr.winner td {{ color: #3fb950; font-weight: 600; }}
    .section-title {{ color: var(--accent); margin: 2rem 0 1rem; font-size: 1.3rem; }}
    .card p {{ line-height: 1.6; color: #8b949e; margin-bottom: 0.5rem; }}
    .explainer {{ background: var(--card); border-radius: 8px; padding: 1.5rem; margin-bottom: 1.5rem; }}
    .explainer dl {{ display: grid; gap: 1rem; }}
    .explainer dt {{ color: var(--accent); font-weight: 600; font-size: 1rem; }}
    .explainer dd {{ color: #8b949e; line-height: 1.6; margin: 0; }}
    .explainer code {{ background: #21262d; padding: 0.15em 0.4em; border-radius: 3px; font-size: 0.9em; }}
    .compare {{ width: 100%; border-collapse: collapse; margin: 0.5rem 0 1.5rem; }}
    .compare th, .compare td {{ padding: 0.6rem 0.75rem; text-align: center;
                                 border-bottom: 1px solid var(--table-border); font-size: 0.9rem; }}
    .compare th {{ color: var(--accent); font-weight: 600; }}
    .compare .row-label {{ text-align: left; color: var(--fg); font-weight: 500; }}
    .compare .yes {{ color: #3fb950; }}
    .compare .partial {{ color: #d29922; }}
    .compare .no {{ color: #8b949e; }}
    .summary {{ color: #8b949e; line-height: 1.6; margin-top: 0.75rem; }}
    footer {{ margin-top: 2rem; color: #484f58; font-size: 0.85rem; }}
  </style>
</head>
<body>
  <h1>Vortex Dashboard</h1>
  <p>Competitive benchmarks against libuv, libevent, and Boost.Asio</p>
  <div class="grid">
{cov_section}
{bench_html}
  </div>

  <h2 class="section-title">What the Numbers Mean</h2>
  <div class="explainer">
    <dl>
      <dt>Post-to-Execute Latency</dt>
      <dd>Time from when a thread calls &ldquo;post work&rdquo; to when that work
      actually runs on the event loop thread. This is the most important metric for
      real-time systems &mdash; it determines how quickly your loop reacts to
      external requests. Lower is better.</dd>

      <dt>Post Throughput</dt>
      <dd>How many posted callables the loop can dispatch per second under sustained
      load from a single producer. Measures raw event dispatch overhead. Note: libuv
      coalesces multiple <code>uv_async_send</code> calls into a single wakeup,
      which inflates its throughput number but means individual posts may be delayed.
      Higher is better.</dd>

      <dt>Timer Jitter</dt>
      <dd>Mean deviation from the requested timer interval. A 1&thinsp;ms repeating
      timer that fires at 1.017&thinsp;ms, 0.983&thinsp;ms, 1.012&thinsp;ms has low
      jitter. This matters for periodic tasks like sensor polling, heartbeats, and
      frame scheduling. Lower is better.</dd>

      <dt>FD Reaction Time</dt>
      <dd>Time from a byte being written to a pipe until the read-side handler fires
      on the event loop. Measures the I/O polling fast path &mdash; how quickly the
      loop wakes from epoll and dispatches your handler. Lower is better.</dd>
    </dl>
  </div>

  <h2 class="section-title">Why Vortex?</h2>
  <div class="explainer">
    <table class="compare">
      <tr>
        <th></th>
        <th>Vortex</th>
        <th>libuv</th>
        <th>libevent</th>
        <th>Boost.Asio</th>
      </tr>
      <tr>
        <td class="row-label">Language</td>
        <td>C++17</td>
        <td>C</td>
        <td>C</td>
        <td>C++11/14/17/20</td>
      </tr>
      <tr>
        <td class="row-label">API style</td>
        <td>Simple OOP &mdash; one class, six methods</td>
        <td>Handle-based C callbacks</td>
        <td>Handle-based C callbacks</td>
        <td>Proactor pattern, completion handlers, coroutines</td>
      </tr>
      <tr>
        <td class="row-label">Thread-safe posting</td>
        <td class="yes">&#10003; Native</td>
        <td class="partial">&#9711; uv_async (coalesced)</td>
        <td class="no">&#10007; Requires socketpair workaround</td>
        <td class="yes">&#10003; boost::asio::post</td>
      </tr>
      <tr>
        <td class="row-label">Compile time</td>
        <td class="yes">Fast &mdash; single header + one .cpp</td>
        <td class="yes">Fast &mdash; small C library</td>
        <td class="yes">Fast &mdash; small C library</td>
        <td class="no">Slow &mdash; heavy template instantiation</td>
      </tr>
      <tr>
        <td class="row-label">Dependencies</td>
        <td class="yes">None (just pthreads)</td>
        <td class="yes">None</td>
        <td class="yes">None (optional OpenSSL)</td>
        <td class="no">Boost headers (~500 MB installed)</td>
      </tr>
      <tr>
        <td class="row-label">Binary size impact</td>
        <td class="yes">~8 KB .text</td>
        <td class="partial">~200 KB shared lib</td>
        <td class="partial">~300 KB shared lib</td>
        <td class="no">~100&ndash;500 KB (template bloat)</td>
      </tr>
      <tr>
        <td class="row-label">Built-in networking</td>
        <td class="no">No &mdash; pure event loop</td>
        <td class="yes">TCP, UDP, pipes, TTY, DNS</td>
        <td class="yes">TCP, HTTP, DNS, RPC</td>
        <td class="yes">TCP, UDP, SSL, serial, pipes</td>
      </tr>
      <tr>
        <td class="row-label">Learning curve</td>
        <td class="yes">~30 min</td>
        <td class="partial">~2 hours</td>
        <td class="partial">~2 hours</td>
        <td class="no">~1&ndash;2 days</td>
      </tr>
      <tr>
        <td class="row-label">Best for</td>
        <td>Embedded, RTOS, IPC, lightweight services</td>
        <td>General-purpose async I/O, Node.js addons</td>
        <td>Network servers, proxies</td>
        <td>Complex async C++ applications, protocol stacks</td>
      </tr>
    </table>
    <p class="summary">
      <strong>Choose Vortex</strong> when you need a dead-simple event loop with
      minimal footprint &mdash; post work from any thread, watch file descriptors,
      schedule timers, and nothing else. No DNS resolvers, no HTTP parsers, no
      template metaprogramming. Just a loop.
    </p>
    <p class="summary">
      <strong>Choose the others</strong> when you need built-in networking (TCP/UDP/SSL),
      protocol-level abstractions, or ecosystem integrations (Node.js, Boost libraries).
    </p>
  </div>

  <h2 class="section-title">About the Libraries</h2>
  <div class="grid">
    <div class="card">
      <h2>Vortex</h2>
      <p>Lightweight C++17 event loop with thread-safe posting, fd source watching,
      and timer support. Single-header design, epoll/kqueue/IOCP backends.
      Built for simplicity and low latency in embedded and server applications.</p>
      <a href="https://github.com/Mrunmoy/Vortex">GitHub &rarr;</a>
    </div>
    <div class="card">
      <h2>libuv</h2>
      <p>The event loop behind Node.js. Cross-platform async I/O library supporting
      epoll, kqueue, IOCP, and event ports. Battle-tested in production at massive scale.
      C API with broad ecosystem support.</p>
      <a href="https://libuv.org">libuv.org &rarr;</a>
    </div>
    <div class="card">
      <h2>libevent</h2>
      <p>Event notification library powering Tor, Chromium, and Memcached.
      Provides abstractions over select, poll, epoll, kqueue. Includes buffered I/O,
      HTTP server, and DNS resolver. Mature C library (since 2002).</p>
      <a href="https://libevent.org">libevent.org &rarr;</a>
    </div>
    <div class="card">
      <h2>Boost.Asio</h2>
      <p>The de facto C++ async I/O library and basis for the C++ Networking TS.
      Proactor pattern with completion handlers, coroutine support, and extensive
      protocol coverage (TCP, UDP, SSL, serial). Part of Boost since 2005.</p>
      <a href="https://www.boost.org/doc/libs/release/doc/html/boost_asio.html">Boost docs &rarr;</a>
    </div>
  </div>

  <footer>Auto-generated by CI &mdash; results from GitHub Actions runners</footer>
</body>
</html>"""
    with open(os.path.join(OUT_DIR, "index.html"), "w") as f:
        f.write(index_html)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    has_coverage = copy_coverage()
    coverage_pct = get_coverage_pct()
    bench_data = load_benchmarks()

    write_badge_json(coverage_pct)
    generate_index(has_coverage, coverage_pct, bench_data)

    print(f"Pages content generated in {OUT_DIR}/")
    if coverage_pct is not None:
        print(f"  Coverage: {coverage_pct:.1f}%")
    if bench_data:
        print(f"  Benchmarks: {len(bench_data['results'])} categories")


if __name__ == "__main__":
    main()
