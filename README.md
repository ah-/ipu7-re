# Intel IPU7.5 (Panther Lake) ISP — reverse-engineering notes

Driving the **hardware ISP** of an IPU7.5 laptop camera without Intel's closed userspace
libraries. Target: Lenovo ThinkPad X1 Carbon Gen 14 (Panther Lake, PCI `0xb05d`), Sony IMX471
front camera (`TBE20A0`, Intel config name `imx471-uf`), Fedora 45, kernel 7.2.15.

Status (2026-11-06): a ~800-line C program (`live2.c`) streams raw frames from the sensor through
the ISP firmware with **its own** auto-exposure, white balance, tone mapping, lens shading strength,
denoise strength and hardware crop/zoom, at 30 fps, rendering close to Intel's stack. It still needs
Intel's out-of-tree IPU7 kernel modules and recorded register templates for the blocks we have not
decoded (noise reduction internals, per-hue colour matrices, program/system buffers).

Method: an `LD_PRELOAD` shim on Intel's HAL recording the kernel interface traffic, one-variable
experiments through `icamerasrc` properties, the HAL's own debug logs, a self-built HAL with small
instrumentation patches, synthetic raw input fed directly to the firmware, and the layout headers
Intel publishes in `intel/ipu7-camera-hal` (`modules/ipu_desc/ipu75xa/`). No Intel binaries were
disassembled.

## Install (Fedora)

```sh
sudo dnf copr enable ahahah/ipu7-camera
sudo dnf install ipu7-camera      # pulls in ipu7-ptl-dkms and the ipu7 libcamera build
sudo reboot
```

`cam -l` should then show the camera on pipeline handler `ipu7`. Remove with
`sudo dnf remove ipu7-camera ipu7-ptl-dkms && sudo dnf distro-sync 'libcamera*'`.
See [Packaging](#packaging) for how the packages track Fedora updates.

Licensing: the tools and packaging are MIT (`LICENSE`). `graph/`, `bundles/` and `zoom2x/` are
register images recorded from Intel's IPU7 camera stack (`intel/ipu7-camera-hal`,
`intel/ipu7-camera-bins`) and remain subject to Intel's terms; the libcamera patches are
LGPL-2.1-or-later, the kernel drivers GPL-2.0.

---

---

## Contents

1. [Architecture](#1-architecture)
2. [Getting Intel's stack running (for recording)](#2-getting-intels-stack-running-for-recording)
3. [Kernel interface (PSYS uapi)](#3-kernel-interface-psys-uapi)
4. [The processing graph](#4-the-processing-graph)
5. [LBFF parameter payload](#5-lbff-parameter-payload-node-0-terminal-0)
6. [Lens shading](#6-lens-shading-lbff-terminal-8)
7. [Hardware statistics](#7-hardware-statistics)
8. [Sensor](#8-sensor-imx471)
9. [Resolutions, modes and hardware crop/zoom](#9-resolutions-modes-and-hardware-cropzoom)
10. [The open pipeline: `live2`](#10-the-open-pipeline-live2)
11. [Tools and experiment techniques](#11-tools-and-experiment-techniques)
12. [Hazards and gotchas](#12-hazards-and-gotchas)
13. [Open items](#13-open-items)
14. [File index](#14-file-index)

---

## 1. Architecture

```
IMX471 ──CSI-2──▶ ISYS (capture) ──raw RG10──▶ PSYS firmware ──▶ NV12
 sensor            /dev/video0                  /dev/ipu7-psys0
                                                node 0 LBFF: Bayer → YUV (all colour/tone work)
                                                node 1 BBPS: temporal NR + output formatting
```

| Piece | Where it comes from | Open? | Needed? |
|---|---|---|---|
| Sensor driver `imx471` | upstream kernel | yes | yes |
| ISYS capture `intel-ipu7-isys` | upstream staging **or** Intel out-of-tree | yes | yes |
| PSYS `intel-ipu7-psys` | Intel `ipu7-drivers` (out-of-tree only) | yes (GPL) | yes |
| ISP firmware `ipu7ptl_fw.bin` | linux-firmware (1.2.1 works) | redistributable blob | yes |
| HAL `libcamhal` + graph descriptions | Intel `ipu7-camera-hal` | yes (Apache) | **no** (only for recording) |
| `libia_aiq` / `libia_aic` / `.aiqb` tuning | Intel `ipu7-camera-bins` | **closed** | **no** — replaced by `live2` |

The upstream kernel only has ISYS (capture) in staging. libcamera on upstream uses its CPU/GPU
software ISP; this project uses the real hardware ISP instead.

---

## 2. Getting Intel's stack running (for recording)

Needed only to record templates and run experiments. Everything below lives in a scratch build
(`s2/` in the session scratchpad); nothing is installed system-wide.

**Kernel modules.** The PSYS module cannot be loaded next to the in-kernel staging driver: Fedora
enables `CONFIG_DEBUG_FS`, which shifts `struct ipu7_device`, and the out-of-tree `ipu7_bus_device`
has extra fields PSYS writes to (memory corruption; it stalls in deferred probe if you are lucky).
Build Intel's **whole trio** instead:

```sh
make KV_IPU7_ISYS=999.0.0 BUILD_INTEL_IPU_ACPI=1     # intel/ipu7-drivers + xps-fedora patch 0101 (hide LT6911)
# without BUILD_INTEL_IPU_ACPI isys never binds the sensor ("no subdevice info provided")
insmod ipu-acpi-common.ko ipu-acpi-pdata.ko ipu-acpi.ko intel-ipu7.ko intel-ipu7-isys.ko intel-ipu7-psys.ko
chmod 666 /dev/ipu7-psys0
```

Swap/restore scripts: stop wireplumber first, `rmmod` the staging pair, `insmod` the above; restore
with `rmmod` + `modprobe intel_ipu7_isys`. A reboot always restores the in-kernel drivers.

**HAL version.** Intel's *current* `ipu7-camera-hal` cannot run IMX471: its graph file
`IMX471_BBG803N3.IPU75XA.bin` carries static-graph hash 1677127292, but code from 2026-06-29 on
expects 929000173 ("failed to init graph reader"); only OV08X40/OV13B10 graphs were regenerated.
Working combination: **HAL `cdc01d4` (2026-06-25) + bins `d235697` + icamerasrc `fe01f98`**.

**HAL config fixes.** `CAMERA_CFG_PATH` must point at `…/etc/camera/ipu75xa/` (the subdirectory).
Intel's sensor JSONs contain trailing commas (`imx471-uf.json` breaks jsoncpp) — strip them.

**Running it.**
```sh
LD_LIBRARY_PATH=$S/usr/lib64 GST_PLUGIN_PATH=$S/usr/lib64/gstreamer-1.0 CAMERA_CFG_PATH=$S/etc/camera/ipu75xa/ \
  gst-launch-1.0 icamerasrc device-name=imx471-uf ! video/x-raw,format=NV12,width=1920,height=1080 ! …
```
With the Intel stack loaded, hide libcamera and the raw ISYS nodes from wireplumber
(`~/.config/wireplumber/wireplumber.conf.d/90-intel-isp-no-libcamera.conf`, see §12).

**HAL instrumentation patches** (scratch build only):
- `IpuPacAdaptor::decodeStats` — env `RGBS_DUMP_DIR`: dumps Intel's decoded RGBS grid per frame.
- `PipeManager::prepareIpuParams` — env `PTZ_PLAN="seq,l,t,r,b;…"`: scripted zoom regions; also
  initialises the previous PTZ state correctly (see §9 for the Intel bug this works around).
- `cameraDebug=4` logs per-frame AE exposures/gains, AWB gains, CCM and gamma/tone samples.

---

## 3. Kernel interface (PSYS uapi)

`include/uapi/linux/ipu7-psys.h` (GPL), copied as `ipu7-psys.h`.

```
GRAPH_OPEN(graph_info with nodes, terminals, links)       → graph_id
per buffer:  GETBUF(userptr) → dma-buf fd ;  MAPBUF(fd)
per frame, per node:  TASK_REQUEST(graph, node, frame_id, terminal buffers)  →  poll + DQEVENT
GRAPH_CLOSE
```

- The HAL passes ioctl numbers as `int` (sign-extended) — an interposer must compare `(unsigned int)req`.
- Payload dma-bufs cannot be `mmap`ed; read them through the userptr recorded at `GETBUF`.
- `frame_id` is per node and must wrap as **`uint8_t`** (0…255). Any other wrap → event error 2 (FRAME).
- Raw input can be zero-copy: V4L2 MMAP buffers → `VIDIOC_EXPBUF` → `MAPBUF` → terminal buffer.
- Task buffer flags: `DMA_HANDLE | NO_FLUSH` (0x18) for inputs and params, 0x10 for the NV12 output.
- Node 0 and node 1 run back to back per frame with the same `frame_id`. Running node 0 alone for
  more than ~3 frames wedges the firmware ("wait running fw tasks clear timeout"); it recovers.

`replay.c` proves the point: one recorded frame through both nodes, error 0, output ≈ Intel's.

---

## 4. The processing graph

IMX471, 1080p video, graph ID 100002 (`graph/graph_info.bin`, `graph/graph_nodes.bin`).
Terminal names from `Ipu75xaTerminalDescriptorAutogen.cpp`.

**Node 0 — LBFF** (raw Bayer → YUV)

| term | size (B) | name | role |
|---|---|---|---|
| 0 | 28288 | `LOAD_ALGO_CACHED` | **register images of all LBFF blocks** (§5) |
| 1 | 896 | `LOAD_ALGO_FRAG_SEQ` | fragment sequencer program (geometry, crop — §9) |
| 2 | 2688 | `LOAD_SYSTEM` | system config (frame/buffer geometry — §9) |
| 5 | 4251456 | `MAIN_DATA_INPUT` | raw RG10 1928×1088, 3904 B/line |
| 8 | 17920 | `LSC_INPUT` | lens-shading table (§6) |
| 10 / 11 / 12 / 13 | 8192 / 1856 / 32640 / 4096 | `AE` / `AF_STD` / `AWB_STD` / `AWB_SAT` | **hardware statistics** out (§7) |
| 18 / 19 | 398912 / 6226560 | `ME_OUTPUT` / `PS_OUTPUT` | intermediate YUV → node 1 terms 7 / 9 (packed/tiled) |

**Node 1 — BBPS** (temporal noise reduction, output)

| term | size (B) | role |
|---|---|---|
| 0 / 1 / 2 / 3 | 2176 / 64 / 1728 / 2624 | params / program / system / per-frame setup |
| 5, 6, 10, 11 | | TNR history inputs |
| 8, 12, 13 | | TNR history outputs |
| 14 | 3113280 | **NV12 1920×1080 output, limited range (16–235)**, `OFS_DP` |

Delayed links feed history: 12→10, 13→5, 8→6 (8→11 same frame). Implemented as ping-pong buffers:
per frame `cur/prev`, term 12 = ref[cur] out, term 10 = ref[prev] in, and likewise for 13/5 and 8/6.

The device-enable bitmap of node 0 (`deb = d41bf1f1 00001fff`) enables LBFF device IDs
0,4–8,12–17,19,20,26,28,30–44 (IDs in `ipu_manifest_db_ipu7_psys_cb_lbff_dev_ids.h`).

---

## 5. LBFF parameter payload (node 0, terminal 0)

The payload is the **literal register contents** of each hardware block, laid out by
`ipu_manifest_db_ipu7_psys_cb_lbff_descriptors.h` (`lbff_0`, 60 sections: device, register offset,
size, payload offset → `lbff_sections.json`). Offsets below are payload byte offsets.

### Decoded blocks

| Block @offset | Format | Notes |
|---|---|---|
| **WB_1_1** @0x2f2c | u32 `[0]=Gr [1]=Gb [3]=B [4]=R`, Q3.12 (4096 = 1.0) | matches HAL-logged gains exactly |
| **CCM_3A_2_0** @0x2990 | stats colour path, 34×u16 | see table below |
| **ACM_1_1** @0x4754 | word 0 enable (24577 on, 1025 bypass); knot tables; from u16 word 78: **24 hue sectors × 3×3 matrix**, Q11, 15-bit two's complement | forced identity / swap / diag(2,1,1) reproduce exactly; Intel tunes each sector in auto |
| **CSC_1_1** @0x680c | RGB→YUV BT.709, s15 Q14 as u16, rows Y/Cb/Cr (3 coeffs + pad), then offsets (2048 = 128 @12 bit) | saturation = scale of the Cb/Cr rows |
| **GammaTM_V4** @0x4b9c | stream of **15-bit samples, 2 per u32** (bits 0–14, 15–29; 30–31 preserved) | samples 0–26 header/knots; **27–1562 gamma LUT** (1536, 0…32767); **1585–3632 tone-gain LUT** (2048, Q12 gain per input level) |
| **B2I_DS_1_1** @0x6830 | scaler: word 0 enable, kernel (§9) | |
| **UpScaler_1_0** @0x6a40 | last u32 (@0x6e4c) = scale step, Q16 | §9 |
| **GLTM_2_0** @0x3388 | static, weak (below) | not an HDR control |

**CCM_3A_2_0** (feeds the AE histograms, §7):

| u16 words | meaning |
|---|---|
| 0 | enable |
| 2–5 (copy 6–9) | **stats white-balance gains** Gr, R, B, Gb — unsigned Q8 (256 = 1.0). Separate from WB_1_1. |
| 10–21 | **3×4 matrix**, rows R′,G′,B′ × cols Gr,R,B,Gb, Q7 (128 = 1.0), 10-bit two's complement; green coefficient split over Gr/Gb; equals the HAL-logged CCM |
| 22–25 | luma weights Gr, R, B, Gb, Q7 (identity = 32 each) |
| 26–33 | channel index config |

**Tone mapping.** Intel's tone-gain LUT is global: ≈3.07× in the shadows falling to ≈1.03× in the
highlights, recomputed per frame. Intel's "WDR level" acts on this LUT. Dark-scene templates contain
**0×** gains for the noise floor — scale this LUT in the log domain (`g^s`), never linearly.

**GLTM_2_0** (local tone mapping block): identical in every recording (all gains, WDR 0/100/200,
all scenes), enabled (device 32). Layout: words 0–218 a Q12 gain curve (1.0 → ~0.06), 386–399 a
scalar + knot pairs (32,516)(512,997)(1024,1254)(1536,1383)(2560,1511), 400–413 scalars. In-run
A/B of curve ×0.25, flat curve, zeroed knots and each zeroed scalar changes the output by ≤1–3
levels. Not a useful knob.

### Noise reduction (BNLM_3_3, XNR_5_2, GD_DPC_2_2, Linearization, BLC)

Not decoded word by word; they track sensor gain. Across the gain templates (1× → 32×):

| register | trend | likely meaning |
|---|---|---|
| BNLM +0x0c/+0x10/+0x12 | 182 → 595 | noise sigma / threshold |
| BNLM +0x1c | 7453 → 5654 | detail preservation |
| BNLM +0x02, +0x18 | 256 → 768, 4 → 8 | search size / iterations |
| XNR reg+0x20 +0x00 | 4877 → 536 | detail retention |
| XNR reg+0x4 +0x08 | 2047 → 633 | blend |
| GD_DPC thresholds | jump only at ≥16× | defect-pixel aggressiveness |
| BLC / Linearization | small shifts | black level vs gain |

Many words are bitfields (non-monotonic), so per-word interpolation is unsafe. Strength control =
copy BNLM+XNR from the template *n* stops up/down (`LIVE_NR`). Measured: +2 → flat-area noise −43 %,
edge detail −13 %; −2 → noise +213 %, detail +5 %.

### Experiment → block map

saturation → CSC · contrast → GammaTM gamma · sharpness → XNR/BNLM · sensor gain →
Linearization/BNLM/XNR/DPC/BLC · white balance / CCM → ACM + tone LUT (+ CCM_3A) · WDR → tone LUT.

---

## 6. Lens shading (LBFF terminal 8)

| | |
|---|---|
| Layout | **35 rows × 64 points × {Gr, R, B, Gb}**, u16, channels interleaved per point (row stride 256 u16) |
| Geometry | point (r, c) at pixel **(32c, 32r)**, origin top-left, **bilinear** (±32 px); x ≥ 1928 is padding |
| Gain | **1 + v / 2048** (verified 0→1.0, 1024→1.5, 2048→2.0, 4096→3.0, 8192→5.0, 12000→6.859) |
| Register block | `LSC_1_2` @0x2274: only word 0 = 0x14100 |

Statistics are computed **after** lens shading. Intel's tables: ~1.0 at the centre, ~5.5× in the
image corners (5.6× at the clamped extremes); identical across sensor-gain templates; in very low
light (~59× total gain) Intel scales (gain − 1) by ~0.86. Colour shading (R/G, B/G) varied ≤4 %
between the lights seen.

Decoded with synthetic input: flat tables (gain law), single grid points (position/footprint),
single channels (order).

**Own calibration** (`calib_lsc.py`): capture with `LSC_ZERO=1 LIVE_DUMP=<n>` while the lens sees a
flat field — a phone screen showing white, pressed against a sheet of paper over the lens (a torch on
paper is *not* flat: 9× hotspot). Use one frame; the light must not change. 2D log-polynomial fit
per channel (residual ~2.4 %), radial extrapolation beyond r = 0.98. Result matches Intel within
±5 % to the image edge; extreme corners ~7× vs Intel's ~5.5×. On a flat field: corner/centre 1.03
(Intel) vs 1.06 (own). `lsc_own_phonescreen.bin`, load with `LSC_FILE=`. Intel's table is the default.

---

## 7. Hardware statistics

Written by LBFF every frame. Verified **bit-exact** against Intel's decoded `cca_out_stats`.

| term | content |
|---|---|
| **12 AWB_STD (RGBS grid)** | **60 × 68 blocks of 32 × 16 px**, 4 × u16 per block = **Gr, R, B, Gb**. Intel's 8-bit `rgbs_grid_block` = value >> 8. **After lens shading, before white balance** → AWB is open-loop. |
| **13 AWB_SAT** | per-block saturation, u8 (4080 of 4096 used) |
| **10 AE** | 4 × 256-bin u32 histograms (1,100,544 samples each), **linear, bin = value >> 8**. `g = (Gr,R,B,Gb) × stats gains`; h0/h1/h2 = rows R′/G′/B′ of the CCM_3A matrix applied to `g`; h3 = luma weights · `g`. Model predicts all four within 2–5 %. |
| 11 AF_STD | not decoded (fixed-focus lens) |

Intel's AE exposes low: on a lit-room frame the centre-weighted linear mean was ≈7.7 % of full scale
(4700/65535) with p99 ≈31 %, the tone curve lifting the image afterwards.

---

## 8. Sensor (IMX471)

| | |
|---|---|
| Pixel array | 4656 × 3496 active (16 MP), native 4672 × 3512 |
| Only driver mode | window 4640×2644, **2×2 binning**, digital crop 1928×1088 → uses a 3856×2176 area |
| Link | 200 MHz × 4 lanes, RAW10 → pixel rate 160 Mpix/s |
| Analogue gain | code g → **1024 / (1024 − g)**, max 960 = 16× |
| Digital gain | 256 = 1×, max 4095 |
| Timing | VBLANK 1203 → 30 fps, exposure ≤ VBLANK + 1070 lines; VBLANK 3500 ≈ 15 fps |
| Test pattern | `V4L2_CID_TEST_PATTERN` has no effect on this module |

**4K.** Intel's 3840×2160 outputs are ISP upscales of the 1080p readout. A native mode needs a new
driver mode (binning off, 3840×2160 window): ~15–16 fps at the current link, no Intel ISP geometry
for that input, noisier in low light — but real detail for crop/zoom (2× without upscaling).

---

## 9. Resolutions, modes and hardware crop/zoom

**Graph file.** `graphtool/listgraphs.cpp` (Intel's open reader) on `IMX471_BBG803N3.IPU75XA.bin`:
55 configurations, **one sensor mode** (1928×1088), **two graph IDs** — 100002 (ISP, all processed
outputs 640×360…3840×2160, single or preview+video dual output via OFS_MP/OFS_DP) and 100026 (raw).

**Changing resolution** keeps the topology; it changes terminal sizes, LBFF term 2 (padded frame
size, every stats/LSC buffer size, grid strides, scaler words), LBFF term 1 (fragment geometry),
BBPS terms 0–3 and the stats/LSC grid dimensions (720p: RGBS 32640 → 30912 B, LSC 17920 → 18048 B).
At 720p Intel already scales inside LBFF. Practical route: record graph + terms 1/2 + BBPS per mode
once, generate image parameters ourselves.

**Hardware crop/zoom** (working, per frame). Intel's path: zoom region → `PipeManager` → `PtzInfo`
→ `GraphResolutionConfigurator::updateStaticGraphConfig` → libia_aic.
*Intel bug:* `PtzInfo()` defaults `zoomCentered = true`; on graphs without zoom-key resolutions
(IMX471) every PTZ request fails ("Update resolution for PTZ fail") until the previous state is
initialised as non-centred.

Encoding (graph, tasks and the 1920×1080 output unchanged):

| where | field |
|---|---|
| LBFF term 1 +0x342 / +0x346 | crop **x** (aligned to 64) / **y** (aligned to 16) |
| LBFF term 1 +0x34a, +0x354, +0x360, +0x364 | crop **width** |
| LBFF term 1 +0x348, +0x350, +0x362, +0x366 | crop **height** |
| LBFF term 1 +0x358 / +0x35c | scaled size 1080 / 1920 |
| LBFF term 2 +0x924 / +0x926, +0x92c / +0x92e | output 1920×1080 (full mode: padded 1984×1088); flags 0 |
| B2I_DS @0x6830 | word 0 = 1; u16 4…131 and 132…259 = two copies of **k[i] = 1024·r − 16·r²·i** for i < 64/r |
| UpScaler last u32 | **r in Q16** |
| BBPS term 1 +0x0 / +0x2 | x + 1 / y |

with **r = crop / output, 0.5 ≤ r ≤ 1** (max 2×). Hard rules (the firmware hangs otherwise):
crop exactly **16:9** (width multiple of 32, height = width·9/16); kernel and step computed from the
**rounded** crop; crop inside the real 1920×1080 image (the 1984 padding gives a garbage stripe).
Zoom state templates in `zoom2x/` (the UpScaler block must be copied too). Verified: fixed 1×, 1.33×,
1.67×, 2×; pan to all corners; a 240-frame continuous zoom+pan animation.

---

## 10. The open pipeline: `live2`

```sh
gcc -O2 -o live2 live2.c -lm
./live2 bundles graph 0 | gst-launch-1.0 fdsrc fd=0 blocksize=3110400 ! \
    rawvideoparse format=nv12 width=1920 height=1080 framerate=30/1 ! queue ! \
    pipewiresink mode=provide stream-properties='props,media.class=Video/Source,node.name=open-isp'
# autoframe --source open-isp
```

Per frame: ISYS raw → LBFF → BBPS, parameters generated from gain-indexed templates
(`bundles/g{0,6,12,18,24,30}` = 1×…32× total gain, nearest in log2 with hysteresis) plus:

| function | implementation |
|---|---|
| **AE** | meters the RGBS grid (Intel's `using_rgbs_for_aec`): per-block luminance with current WB, centre-weighted, target 4700/65535, p99 ≤ 45000, saturation map + h3 histogram highlight protection (clip > 0.5 % or p99.5 bin > 235 → back off), log-domain damping every 3rd frame (2-frame sensor latency) |
| exposure policy | exposure first; VBLANK 1203 → 3500 (30 → 15 fps) before gain > 4×; total gain ≤ 32× (16× analogue × 2× digital); stops chasing at the limit |
| **AWB** | RGBS grid, 2-pass grey world with neutral-block outlier rejection, skips dark/saturated blocks, holds in darkness, warm bias under warm light; writes gains to WB_1_1 **and** CCM_3A words 2–9 |
| **tone** | tone-gain LUT scaled `g^s` (adaptive shadow lift), gamma LUT scaled (adaptive white point → p99 ≈ 228) |
| **LSC** | template table; strength 1.0 up to 16× gain → 0.86 at ~60× |
| **NR** | template section shift `LIVE_NR` |
| **crop/zoom** | `LIVE_CROP=x,y,r` |

Compared back to back with Intel in the same scene: luminance percentiles within a few levels
(highlights stop at ~228 where Intel clips at 251), slightly less warmth, a bit more shadow noise.

**Environment variables**

| var | effect |
|---|---|
| `LIVE_LOG=1` | per-30-frame log (AE, exposure, WB, tone) |
| `LIVE_AE`, `LIVE_AE_RGBS` | output-Y target / RGBS target |
| `LIVE_TM`, `LIVE_SAT` | shadow-lift / saturation multipliers |
| `LIVE_NR=±n` | denoise strength in template stops |
| `LSC_FILE`, `LSC_ZERO`, `LSC_STRENGTH` | own LSC table / LSC off / override strength |
| `LIVE_CROP=x,y,r` | hardware crop (r = 0.5…1) |
| `LIVE_DUMP=n` | dump raw + stats + params of frames n and n+30 to `statdump/` |
| `LIVE_PATCH="off=val,off:n=val"` | patch LBFF param u16 words (experiments) |
| `LIVE_FREEZE=n`, `LIVE_TOGGLE=k` | freeze all loops at frame n; toggle the patch every k frames |
| `LIVE_CROP_ANIM/_PAN/_STEP` | crop test animations |

---

### libcamera pipeline handler + IPA (working)

`~/Projects/libcamera` (branch `ipu7` on v0.7.2, Fedora's version):

| path | content |
|---|---|
| `include/libcamera/ipa/ipu7.mojom` | IPA interface: init / configure / start / stop / sensorControls, map/unmapBuffers, computeParams, processStats (→ sensor controls + metadata), processOutput; fixed shared-buffer ids |
| `src/libcamera/pipeline/ipu7/` | `ipu7.cpp` handler, `psys.{h,cpp}` PSYS wrapper, `ipu7-psys.h` uapi |
| `src/ipa/ipu7/` | `ipu7.cpp` IPA module (tuning file, buffer mapping), `ipu7_isp.{h,cpp}` 3A + parameter generation |
| `src/ipa/ipu7/data/imx471.yaml` | tuning: `ae.target/gain_max/min_fps`, `tone.shadow_lift_max`, `denoise.template_offset`, `lsc.low_light_strength`, `templates` dir |

Handler: matches media driver `intel-ipu7` only if `/dev/ipu7-psys0` is accessible; one NV12
1920×1080 stream; 12 dma-buf buffers shared with the IPA (8 parameter terminals written by the IPA,
3 statistics + the NV12 output read by it), imported into the PSYS per stream; PSYS-internal
buffers (intermediates, TNR history, AF stats) private. Per frame: `computeParams` → node 0 →
`processStats` (sensor controls applied immediately) → node 1 → copy to the app buffer →
`processOutput`. Synchronous on the pipeline thread.
Data: graph and templates in `<prefix>/share/libcamera/ipa/ipu7/imx471/{graph,bundles}` (copied from
this repo into the install, not committed to libcamera); `LIBCAMERA_IPU7_DATA` overrides; env
`LIBCAMERA_IPU7_NR / _MIN_FPS / _TM_MAX` override tuning values for live experiments.
Build: `meson setup build -Dpipelines=ipu7 -Dipas=ipu7 …; ninja -C build install` (prefix `install/`).
Test: `LD_LIBRARY_PATH=install/lib64 install/bin/cam -c1 -C90`.
PipeWire: runtime drop-in `/run/user/1000/systemd/user/wireplumber.service.d/libcamera-ipu7.conf`
(LD_LIBRARY_PATH, LIBCAMERA_IPA_MODULE_PATH).
Lessons: reset the frame id to 0 for every new graph/stream and advance it even on errors (otherwise
error 2 forever); the mojom generator mishandles calls with no inputs but outputs (give them a
dummy argument) and does not allow extra outputs on `start()`; `CameraSensor::sensorInfo()` fails on
imx471 (no crop selection).

## 11. Tools and experiment techniques

| tool | purpose |
|---|---|
| `psysdump.c` | `LD_PRELOAD` shim: logs GRAPH_OPEN/GETBUF/TASK, dumps terminal buffers. `PSYSDUMP_DIR`, `PSYSDUMP_FROM` (8-task window), `PSYSDUMP_FULL` (all buffers of a task pair) |
| `exp.sh` | run icamerasrc with properties under the shim + `cameraDebug=4` |
| `diffexp.py`, `secdiff.py` | diff experiments per terminal / per LBFF section, masking baseline noise |
| `replay.c` | replay one recorded frame (proof of independence) |
| `live.c` | first live pipeline (fixed params) |
| `synth.c` | feed synthetic or recorded raw through the full graph, dump stats/output; `FLAT_LSC`, `LSC_POINT=r,c,v[,ch]`, `CCM3A="i=v,…"`, `LIVE_PATCH`, `SYNTH_RAW`, `NFRAMES`, `NODE0_ONLY` |
| `tog.sh` | in-run toggle A/B with frozen loops; reports luminance deltas, flat-area noise, edge detail |
| `ab.sh`, `sab.sh` | single-run / synthetic captures |
| `calib_lsc.py` | flat-field LSC table |
| `graphtool/listgraphs.cpp` | list configurations in a static-graph binary |

Techniques that worked, in order of usefulness:
1. **Intel's open layout headers** (terminal descriptors, payload descriptors, device IDs) turn opaque
   buffers into named register blocks.
2. **Forced extreme settings** through Intel's own controls (identity / swapped colour matrices,
   manual gains) — the changed bytes reveal format and scaling.
3. **Intel's own debug output** (logged CCM/gains; patched HAL dumping decoded stats) as ground truth.
4. **Synthetic input** (flat colours, ramps, single LSC grid points) — exact, no light needed,
   camera LED off. Must run the full graph.
5. **In-run toggling** for anything judged on live images: run-to-run drift (people move) is as large
   as most effects.

---

### Suspend (analysed, not fixed yet)

Both IPU7 drivers refuse system suspend while in use: `psys_suspend` returns -EBUSY whenever the
PSYS firmware is running (i.e. while any process has `/dev/ipu7-psys0` open, not only during a
task), and `isys_suspend` returns -EBUSY while a capture stream is open. Planned fix (DKMS 1.1):
- PSYS: on suspend drain in-flight tasks (userspace is frozen; `get_running_fw_task_count`), mark
  open graphs lost, stop the firmware via the runtime-PM path; on resume restart it (after the
  parent's `ipu_buttress_authenticate`). Tasks on a lost graph → -ENODEV, closing one succeeds.
- ISYS: stop the hardware stream on suspend instead of refusing; after resume userspace restarts it.
- libcamera handler: on -ENODEV / firmware error / watchdog timeout / missing raw frames, close and
  re-open the graph, restart capture, reset the frame id.
Needs several real suspend/resume cycles (`sudo rtcwake -m mem -s 15` with the camera open).

### Packaging

Fedora COPR [`ahahah/ipu7-camera`](https://copr.fedorainfracloud.org/coprs/ahahah/ipu7-camera/),
built from this repo; each package has a `.copr/Makefile` (COPR "make_srpm", package subdirectory):

| package | from | contents |
|---|---|---|
| `libcamera*` | `packaging/libcamera/` | Fedora's own dist-git package at the commit in `fedora-commit`, plus `patches/` (ipu7 handler + IPA, async/zero-copy, simple-handler yield), Release `<fedora>.ipu7.<ipu7-release>` |
| `ipu7-ptl-dkms` | `packaging/ipu7-ptl-dkms/` | Intel's ipu7-drivers + Fedora/PTL `dkms.conf` from [`ah-/ipu7-drivers`](https://github.com/ah-/ipu7-drivers) branch `ptl-dkms`; depmod search order; initramfs omits the IPU7 modules |
| `ipu7-camera` | `packaging/ipu7-camera/` | `graph/` + `bundles/` → `/usr/share/libcamera/ipa/ipu7/imx471`, udev `uaccess` for `/dev/ipu7-psys*`, WirePlumber rule hiding the raw ISYS nodes; requires the other two |

Fedora updates: `.github/workflows/track-fedora-libcamera.yml` checks Fedora's `f45` libcamera
branch daily and commits the new commit id to `packaging/libcamera/fedora-commit`; the push
triggers a COPR rebuild, whose release (`N.ipu7.M`) sorts above Fedora's `N`. If the patches stop
applying the COPR build fails, Fedora's build wins on the next update, and the camera falls back
to libcamera's software ISP (likewise if DKMS fails on a new kernel: in-kernel drivers, no PSYS,
the ipu7 handler does not match). New Fedora release: change `f45` in the workflow and
`fedora-commit`. libcamera version bumps need the patches rebased (branch `ipu7` in
[`ah-/libcamera`](https://github.com/ah-/libcamera), `ipu7-fedora` = on top of Fedora's patches).

`packaging/migrate-to-copr.sh` (sudo) replaces a hand-installed setup (the old
`install-system.sh`: DKMS from a checkout, `/etc` config, local RPMs, versionlock) by the COPR
packages. `packaging/install-user.sh` retires the earlier software-ISP tuning in the user session.

## 12. Hazards and gotchas

- **Suspend.** While PSYS streams, system suspend fails (`psys_suspend` returns −EBUSY); the kernel
  retried 154× over 48 min with the lid closed, the laptop never slept, and the stream died. Stop
  the pipeline before sleep and restart after; restarting the pipeline alone recovers it.
- **Always-on test streams** keep the camera LED lit and power drawn.
- **Reboot cleanup.** While experimenting, `90-intel-isp-no-libcamera.conf` hides libcamera and the
  raw IPU7 V4L2 nodes; delete it when going back to the in-kernel drivers, or the camera disappears.
  The `webcam-settings` service (VBLANK writes) must be stopped while the Intel stack or `live2` runs.
- **Measuring images.** The NV12 output is limited range — compare through the same conversion
  (a full-range formula faked a "darker" result). Node 0's PS output is packed/tiled. Synthetic
  full-graph runs are not deterministic (TNR state); compare stats, not node-1 output.
- **Firmware hangs** come from inconsistent geometry (non-16:9 crops, rounded vs unrounded ratios),
  node-0-only runs > 3 frames, or wrong frame-id wrap; they clear after the process exits.
- **Shell details.** `pipewiresrc target-object` wants the node serial and can report "target not
  found" while another client holds the node.

---

## 13. Open items

- Own colour matrices per illuminant into ACM (needs calibration data, e.g. a colour chart) — or
  CCT-indexed ACM templates recorded under a few lights.
- BBPS (temporal NR) parameters; NR blocks parametric instead of templates.
- Generate LBFF terms 1/2 for other modes instead of recording them; native-4K sensor mode.
- Port the PSYS driver into the in-kernel staging ipu7 driver (drop out-of-tree modules).
- libcamera: move 3A into an IPA module + tuning file, asynchronous processing, zero-copy output,
  controls (ScalerCrop → hardware crop), coexistence with the `simple` handler, packaging.
- Autoframe driving the hardware crop through a control channel.
- Report the stale IMX471 graph file to Intel.

---

## 14. File index

| path | content |
|---|---|
| `live2.c` | open pipeline (current) |
| `live.c`, `replay.c` | earlier proofs |
| `synth.c`, `psysdump.c`, `calib_lsc.py`, `*.sh`, `*.py` | tools (§11) |
| `ipu7-psys.h` | PSYS uapi header |
| `lbff_sections.json` | LBFF payload section map |
| `graph/` | recorded 1080p graph (graph_info, graph_nodes) |
| `bundles/g0…g30/` | parameter templates per gain: LBFF terms 0/1/2/8, BBPS terms 0–3 |
| `zoom2x/` | recorded 2× zoom state |
| `lsc_own_phonescreen.bin` | own LSC table |
| `dump-frame151/` | one fully recorded frame (all buffers) |
| `graphtool/` | static-graph lister |
