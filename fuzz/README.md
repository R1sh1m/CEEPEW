# fuzz/ — Off-Host Fuzzing Harnesses

This directory contains standalone fuzzing harnesses for CEE-PEW's
cryptographic and reliability primitives. Each harness is a single C file
that exports `LLVMFuzzerTestOneInput()` for libFuzzer, and is also
compatible with AFL's `__AFL_FUZZ_INIT` approach.

| Target | File | What it fuzzes |
|--------|------|----------------|
| Ascon-128 AEAD | `fuzz_ascon/fuzz_ascon.c` | `crypto_ascon_aead_encrypt` / `decrypt` round-trip |
| SHA-256 | `fuzz_sha256/fuzz_sha256.c` | `crypto_sha256_compute` with arbitrary input |
| Hamming(15,11) | `fuzz_hamming/fuzz_hamming.c` | FEC encode/decode round-trip + single-bit error correction |
| HKDF-SHA256 | `fuzz_hkdf/fuzz_hkdf.c` | `crypto_hkdf_derive` with all params from fuzzer + determinism |
| ARQ state machine | `fuzz_arq/fuzz_arq.c` | `ecc_arq_encode`/`decode` round-trip, duplicate rejection, reset |

## Prerequisites

- Clang with `-fsanitize=fuzzer` (libFuzzer mode) OR
- `afl-clang-fast` (AFL mode)
- The component source files from the CEE-PEW project

## Building (libFuzzer)

From the fuzz target directory, compile the required component source
file(s) and link with the harness:

```powershell
# Example: fuzz_ascon
cd fuzz\fuzz_ascon
clang -fsanitize=fuzzer -I..\..\components\crypto -I..\..\components\ceepew_common `
      -I..\..\components\hal -c ..\..\components\crypto\crypto_ascon.c `
      -o crypto_ascon.o
clang -fsanitize=fuzzer -I..\..\components\crypto -I..\..\components\ceepew_common `
      -I..\..\components\hal fuzz_ascon.c crypto_ascon.o `
      -o fuzz_ascon.exe
```

## Running

```powershell
.\fuzz_ascon.exe -max_len=512 -timeout=5
```

The `-max_len` flag limits input size; `-timeout` kills any slow run.
See [libFuzzer docs](https://llvm.org/docs/LibFuzzer.html) for more
options.

## Adding a new harness

1. Create `fuzz/fuzz_<name>/fuzz_<name>.c`.
2. Include the project headers.
3. Define `int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)`.
4. Add the entry to the table in this README and to the project root
   `README.md`.
