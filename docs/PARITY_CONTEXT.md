# MT6737T ROM -> Nura driver parity program

## Objective
For the assigned driver only, bring the Nura `v7.0.10-mt6735` kernel to behavioral parity with the Samsung/MediaTek driver in the SM-G532M Android 6.0.1 ROM. Deliver a reviewable patch, compile-test it, and report exactly what is implemented, what is inferred, and what still needs hardware.

This is a driver-porting exercise, not an upstream-submission exercise. Prefer a small, correct, compile-tested patch over a speculative rewrite.

## Fixed paths
- ROM kernel ELF with symbols: `$M/zip/vmlinux.elf` (32-bit ARM, recovered kallsyms)
- ROM symbol list: `$M/zip/symbols.txt`
- ROM strings: `$M/zip/vmlinux.strings`
- ROM boot image: `$M/zip/boot.img`
- ROM system image: `$M/zip/system.raw.img`
- Nura reference config: `$M/nura.config`
- Downstream Samsung GPL kernel source: `$M/src/downstream-grandpplte`
- Nura kernel base: `$M/src/nura-linux`
- Assigned Nura worktree: `$M/worktrees/<driver>`
- Patch output: `$M/patches/<driver>.patch`
- Report output: `$M/logs/<driver>.md`
- Build tool shims: `$M/bin` (prepend to PATH)

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## Sources of truth, in order
1. Downstream Samsung/MTK source: `$M/src/downstream-grandpplte`. It is the closest available source to the ROM binary.
2. ROM `vmlinux.elf` symbols/disassembly/strings for register values, firmware names, GPIOs, timing, and behavior that the downstream tree does not match.
3. Nura worktree: current driver and DTS.
4. Linux mainline patterns for API/style only.

Do not invent register values. If a value comes from downstream source, cite the file/line in the report. If it comes from ROM disassembly, cite symbol + address. If inferred, mark `[INFERENCE]`.

## Build / compile test
Run from the assigned worktree:

```sh
export PATH="$M/bin:$PATH"
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 \
  AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy \
  OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf \
  STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/path/to/changed_file.o
```

Also compile every other object you touched. Do not run a full kernel build unless asked; the integration owner does that.
If the build system needs a target-specific tool, use the matching `aarch64-linux-gnu-*` binary rather than changing kernel sources.

## Editing rules
- Edit only files owned by your assigned driver plus its private headers, Kconfig/Makefile entries needed to build it, and its DTS nodes if unavoidable.
- Do not edit shared files such as `arch/arm64/boot/dts/mediatek/mt6735.dtsi`, `mt6328.dtsi`, or `mt6737t-samsung-grandpplte.dts` unless the task explicitly says so. If a DTS change is required, put the exact fragment in the report and in a separate `DTS-FRAGMENT` section; do not silently modify shared DTS.
- Do not touch unrelated drivers, coding style, or generated files.
- Preserve GPL notices and copyrights when porting downstream code. Add a clear `Port from Samsung/MTK downstream ...` comment.
- No firmware blobs. If parity depends on a blob, implement the request/load interface and document the missing blob.
- No stubs, fake success paths, or `TODO: implement` in the delivered patch. If a feature cannot be completed without hardware, leave it out and document it as a follow-up.
- `git diff --binary` is the deliverable. The worktree must contain only your intended changes.

## Required report
Write `$M/logs/<driver>.md` with:
1. Assigned driver and files changed.
2. ROM evidence: symbols, strings, downstream files/lines.
3. Parity matrix: behavior, ROM implementation, Nura before, after, status (`done`, `partial`, `deferred`).
4. What is compile-tested and the exact command/result.
5. What is inferred or unverified on hardware.
6. DTS fragment, if any.
7. Patch path.

## Patch output
From the assigned worktree:

```sh
git diff --binary > "$M/patches/<driver>.patch"
```

Do not commit unless asked. Do not run `git checkout` or revert unrelated files.

## Hardware limitation
No phone is attached. Acceptance is: source-level parity for the implemented paths, clean compile, and a documented list of hardware-only unknowns.
