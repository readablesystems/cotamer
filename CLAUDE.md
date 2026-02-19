# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A C++23 coroutine library called **Cotamer** for multithreaded event-driven asynchronous programming, currently useful for simulation but working towards deployment.

## Build Commands

```bash
# Standard build
make

# Build with sanitizers (via GNUmakefile convenience)
make                          # default build
make SAN=1                    # AddressSanitizer + UBSanitizer
make TSAN=1                   # ThreadSanitizer
make V=1                      # verbose output

# Build a single target
make build/cot-test

# Build into a different directory
make BUILD=build-tsan TSAN=1

# Run tests
make test                     # runs cot-test and ctconsensus -q -R 10000
build/cot-test                # coroutine library tests
build/ctconsensus             # consensus simulation
build/ctconsensus -q -R 10000 # quiet mode, 10000 rounds
build/cot-test-threads        # threading tests (use build-tsan/ for race detection)
```

## Architecture

### Cotamer Library (`cotamer.hh`, `detail/cotamer.cc`)
Coroutine framework with deterministic virtual time. Core abstractions:
- `cotamer::task<T>` — coroutine returning T, uses `co_await`/`co_return`
- `cotamer::event` — one-shot signal, awaitable
- `cotamer::after(duration)`, `cotamer::at(time_point)`, `cotamer::asap()` — timer events
- `cotamer::any()`, `cotamer::all()`, `cotamer::attempt()` — event combinators
- `cotamer::loop()` — runs the event loop; `cotamer::clear()` stops it
- `cotamer::now()` — current virtual time

See `COTAMER.md` for the full programming manual.

### Network Simulator (`netsim.hh`)
Simulated network on top of Cotamer, here for testing purposes.
- `channel<T>` — directed link between servers; `send()` is a coroutine with simulated delay
- `port<T>` — receiving endpoint; `receive()` suspends until a message arrives
- `network<T>` — manages channels/ports by integer ID; provides `link(src, dst)` and `input(id)`; includes seeded RNG with `coin_flip()`, `uniform()`, `exponential()`, `normal()` distribution helpers

### Chandra-Toueg Consensus (`ctconsensus.cc`, `ctconsensus_msgs.hh`)
Implementation of the Chandra-Toueg rotating-coordinator consensus protocol. Message types: `PREPARE`, `PROPOSE`, `ACK`, `DECIDE`. The pset involves modifying `netsim.hh` to introduce network faults that break consensus.

### Utilities
- `utils.hh` — string-to-number parsing, random seeding helpers
- `detail/` — internal Cotamer implementation (timer heap, event handles)

## Key Conventions

- C++23 standard required (`-std=gnu++2b`)
- Namespace aliases: `namespace cot = cotamer;`, `using namespace std::chrono_literals;`
- Tasks must be `co_await`ed or `.detach()`ed — otherwise the coroutine is destroyed immediately
- All time is virtual and deterministic; behavior depends only on configuration and random seed
- Server IDs are integers; the special ID `nancy_id = -1` receives final decisions in ctconsensus
