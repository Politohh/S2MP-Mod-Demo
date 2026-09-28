# Keeping the RE tooling off your gaming Windows

**The problem.** Modern kernel anticheats (EAC, BattlEye, Vanguard, Ricochet) scan the
whole machine, not just their own game. IDA, Cheat Engine, DLL injectors and their
drivers are flagged as cheat tooling regardless of which game you were using them on.
Having them installed on the Windows you play other games from is a genuine ban risk
even though nothing here touches those games.

**The goal.** Everything in this project — the game, the mod, IDA, Cheat Engine, the
build — lives somewhere isolated, and the work products (repo, IDB, demos, logs) still
end up on your local PC.

---

## Your hardware, measured

    CPU / RAM        16 logical cores, 32 GB          plenty
    GPU              AMD Radeon RX 9070 XT            <-- this decides everything
    Hyper-V role     NOT installed
    HypervisorPresent=True  -> that is VBS / Credential Guard, not Hyper-V
    Other hypervisors  none (no VMware, no VirtualBox)
    Free space       E: 1965 GB, F: 238 GB, C: 126 GB, D: 137 GB
    Game             E:\SteamLibrary\steamapps\common\Call of Duty WWII

---

## ⚠ THE CONSTRAINT THAT DECIDES THE PLAN — AMD + VM = no GPU

To run WWII inside a VM you need the guest to have a real GPU. There are exactly three
mechanisms and none of them works here:

| mechanism | status on your machine |
|---|---|
| **Hyper-V GPU-P** (GPU Partitioning, what Easy-GPU-PV automates) | **Does not work on AMD.** Works on NVIDIA and Intel; AMD's client driver does not expose the partitioning interface. This is not a configuration problem you can solve. |
| **Hyper-V DDA** (full passthrough) | **Windows Server only.** Not available on Windows 11 Pro. |
| **KVM/Proxmox passthrough** | Works, but needs a Linux host and a **second GPU** for the host to use. You have one GPU. |

VMware Workstation's software 3D (SVGA3D) will technically start a DX11 title, but a
2017 AAA game on an emulated adapter is not something you can test demos or matchmaking
in. Treat it as "not a route".

**Conclusion: the GAME cannot usefully run in a VM on this machine. Anything that tells
you otherwise is going to waste a weekend.**

---

## The three real options

### Option 1 — Separate Windows install (RECOMMENDED)

A second Windows on its own partition or drive, used only for this project.

    Isolation   TOTAL. Your gaming Windows never has IDA, CE, or an injector on it,
                and never even has the registry/driver traces.
    GPU         Full native performance. WWII runs properly.
    Cost        ~80 GB, one install, a boot menu entry.
    Downside    A reboot to switch. That is the entire downside.

This is the correct answer for your stated problem. It is the only option that gives
both full isolation and a working game.

You have 1965 GB free on E: and 137 GB on D: — either can host it, though a separate
physical drive is cleaner than a partition on your games drive.

### Option 2 — Hyper-V VM for the TOOLING half only (works today)

The half that needs no GPU moves into a VM immediately:

    In the VM        IDA Pro + the IDB, Visual Studio / MSBuild, all of tools/*.py,
                     the repo, demo file analysis
    Stays outside    only the game itself + Cheat Engine (CE must attach to the game)

That already removes IDA and the build environment from your gaming Windows, which is
most of the persistent footprint. `tools/vm/New-S2DevVM.ps1` provisions this.

⚠ Enabling the Hyper-V role puts your HOST Windows under a hypervisor too. VBS is
already doing that here, so it changes little for you — but some anticheats do treat
"running under a hypervisor" as suspicious, so this is a trade, not a free win. If that
worries you, use Option 1 instead.

### Option 3 — do nothing, but be disciplined

Lower effort, lower assurance: never run an anticheated game and these tools in the
same boot, and do not install Cheat Engine's kernel driver (DBK). CE without the driver
supports the direct memory reads used by this project.

This reduces risk. It does not remove it, because artefacts persist on disk.

---

## Whichever you pick: getting the work back onto your local PC

`tools/vm/Sync-Workspace.ps1` copies the things that matter back to the host, and it is
the same script for a VM guest or a second Windows install (it just needs a path or a
share to write to).

What it treats as work product, and why:

    repo         the source. Git already protects it, but the branch is not pushed.
    IDB          s2x_dump.exe.i64 — THE most valuable artefact in this project. Months
                 of naming. Note RULE A10: IDA keeps the live DB in loose
                 .id0/.id1/.nam files and only repacks the .i64 on a CLEAN CLOSE, so
                 the sync warns if the loose files are newer than the .i64.
    demos        main/demo/*.demo and demos/*.dm_s2 — recordings cannot be regenerated
                 without replaying the match.
    logs         main/s2mp_console.log — the measurements sessions are built on.
    bans         s2mp_bans.txt

Run it after any session that changed the IDB or produced a recording.

---

## Recommended arrangement

If you take Option 1 (separate Windows), the layout that keeps everything safe:

    Gaming Windows      nothing from this project. Ever.
    Project Windows     Steam + WWII, the mod, IDA, Cheat Engine, Visual Studio
    Shared storage      E: (or a NAS/OneDrive folder) holds the repo, the IDB and the
                        demos, so both installs see the same files and nothing lives
                        only inside one boot

Because your repo already sits in OneDrive
(`C:\Users\joshu\OneDrive\Documents\GitHub\S2MP-Mod`), signing into OneDrive on the
project install gives you the source sync for free. The IDB is far too large for
OneDrive to be sensible — sync that with the script to a fixed path on E:.

---

## Honest status of the scripts in tools/vm/

`New-S2DevVM.ps1` and `Sync-Workspace.ps1` are written but **have not been run**: the
Hyper-V role is not installed on this machine, and installing it needs elevation and a
reboot. They validate their preconditions and refuse rather than half-doing anything,
but per the Fix Declaration Rules they are UNTESTED, not proven.

---

## 2026-08-14 — the two scripts that make Option 1 practical

User restated the requirement plainly: *"I don't want this stuff present when I
play something with Ricochet/Vanguard."* That rules out every VM/sandbox option
(the AMD GPU constraint above is unchanged) and leaves the separate Windows
install. These two scripts close the gap.

### tools/vm/Test-GamingWindowsClean.ps1  — run on the GAMING Windows

Read-only. Answers "is any of this present?" with evidence instead of hope, and
exits 0 clean / 1 dirty so it can gate a scheduled task if you want.

Checks, in order of how much a kernel anticheat cares:

    1. Cheat Engine's DBK KERNEL DRIVER   <- the one that matters most
    2. installed programs (CE, IDA, Hex-Rays, x64dbg, Process Hacker/System Informer)
    3. running processes
    4. the deployed s2mp-mod.dll, and the repo (noted, not counted as tooling)
    5. Run-key autostarts
    6. optional -Deep drive scan for stray copies

VERIFIED 2026-08-14 by running it on the current machine: it correctly reported
DIRTY with Cheat Engine 7.7, IDA Professional 9.1, both running, and the deployed
DLL -- while correctly reporting **no DBK driver** and **no autostarts**, which is
the genuinely reassuring part. Every CE use in this project has been a direct read
at a computed address, so the kernel driver has never been installed.

### tools/vm/Setup-ProjectWindows.ps1  — run ONCE on the NEW Windows

    * REFUSES to run if IDA/CE are already installed, so it cannot be fired at
      the machine you are trying to keep clean. VERIFIED -- it refused on this
      machine and named both products.
    * verifies the shared Steam library and reports what carries over
    * winget-installs git / python / VS build tools
    * prints the three things that genuinely cannot be scripted

### ⭐ The game does NOT need re-downloading

Steam can adopt an existing library folder: Settings > Storage > Add Drive, point
it at `E:\SteamLibrary`. It verifies rather than re-downloads.

And because they live INSIDE the game directory, these come across for free:

    E:\SteamLibrary\steamapps\common\Call of Duty WWII\S2MP-Mod\   botnames, botkits,
                                                                  botemblems, botcards,
                                                                  the LUI + CSV dumps
    ...\Call of Duty WWII\main\demo\                              every recorded .demo

So the only things that actually move between installs are the repo (OneDrive
does it for free) and the IDB (Sync-Workspace.ps1, which honours RULE A10 and
refuses to present a stale .i64 as a good backup).

### The arrangement, end to end

    Gaming Windows    nothing from this project. Verify with Test-GamingWindowsClean.
    Project Windows   Steam + WWII + mod + IDA + CE (NO DBK driver) + VS
    Shared on E:      the Steam library, and therefore the demos and mod data
    Repo              OneDrive
    IDB               Sync-Workspace.ps1 to a fixed path on E:
