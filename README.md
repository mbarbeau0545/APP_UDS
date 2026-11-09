# APP_UDS integration

This implements the diagnostic server, classical CAN ISO-TP, Excel generation,
public business adapters and RAM-only programming validation from
`SPEC_APP_UDS_H7_INTEGRATION.md`. The approved initial profile is **disabled**.
Authentication and maintenance hooks deny by default. No hardware erase, physical
Flash programming, reset or image activation has been enabled.

The protected production bootloaders and hardware acceptance campaigns from lots
7 and 8 of the specification remain separate work. This implementation does not
claim ISO certification or complete G474 board integration.

## Repository audit

| Area | Observed contract | Integration |
|---|---|---|
| Common libraries | `src/0_Common/Library`, `LIBFIFO_*` | Independent `ISO_TP/Src/LIBIsoTp.*`; adapter descriptors use FIFO APIs only |
| Parameters | `APPSPM_GetParam(id, output, exact_size)`, `APPSPM_SetParam(id, const_input, exact_size)` | Generated adapters use the public, hardware-independent `APP_SPM_Access.h` contract |
| Persistence | `APPSPM_SetParam` applies RAM and defers NVM publication | Catalogue explicitly says `APPSPM_NVM_DEFERRED`; success does not promise durable storage |
| Sensors | `APPSNS_Get_SnsValue(id, snapshot)` | Only resolved, read-only FLOAT32 sources; invalid snapshots are refused |
| Actuators | `APPACT_GetActValue` / `APPACT_SetActValue` | Explicit FLOAT32 business bindings, protected by maintenance policy |
| System | `APPSYS_GetEcuPosition` | Explicit read-only `ECU_POSITION` binding |
| CAN | `FMKFDCAN_ConfigureRxItemEvent`, `FMKFDCAN_SendTxItem` | Exact physical software subscription, copied receive descriptors, no HAL handles |
| Active software workbook | `Project_SoftwareCfg_V0.xlsm` | Five new named tables on the UDS worksheet |
| Software version | `GeneralInfoSoftware`: `SoVersion`, **ECU ID** | Explicit `CatalogueVersion=V01Pr01`; an ECU ID is never used as a revision |
| Hardware workbooks | Both families expose `CPU_Config` and `Fdcan_Info` | Family, node and allocated RX/TX resources are validated separately |
| Existing JSON | `SENSORS`, `ACTUATORS`, `DIAGNOSTIC`, `PARAMETERS`, `LOGIC` | Other sections preserved; PARAMETERS refreshed from the same business model as UDS |
| Existing bus rate | FMKFDCAN's private startup table initializes node 1 at **250 kbit/s** | Preserved. The requested UDS profile is 500 kbit/s and requires a coordinated configuration/bus change before enabling |

The initial workbook exposures are SYS_MACHINE_ID (read), SERIAL_NUMBER (read),
HW_VERSION (write-only business parameter), and SW_VERSION (read/write). Writes
remain denied. TEST_PARAM is a structure and is deliberately not exposed without
an explicit field-level serializer. There are no configured sensor or actuator
interfaces in the active workbook; no fictitious business bindings were created.

## Architecture and ownership

`LIB_ISOTP` knows only a copying CAN port and a payload callback. It owns the RX/TX
storage supplied at initialization. `LIBISOTP_Send` copies the caller's payload.
Single, first, consecutive and flow-control frames are bounded to classical CAN;
normal 11-bit and 29-bit addressing are selected by the adapter. RX and TX have
independent progress, sequence and timeout state. FC WAIT/OVERFLOW, block size,
sequence wrap and STmin are handled. Microsecond STmin is rounded up to 1 ms.
Extended addressing and CAN FD are outside this profile.

`APPUDS_OnPayload` copies a single request into server-owned memory. Call it only
from serialized cyclic context; use the queued adapter for framework callbacks.
`APPUDS_ServerInit` arms CFG; `APPUDS_ServerCyclic(now_ms)` enters OPE and performs
bounded service steps for standalone ports and host tests. Request handlers return
`RC_WARNING_PENDING` to preserve the
request and receive later polling calls. P2/P2* responses, a service execution
deadline and S3 expiry are checked with wrap-safe 32-bit elapsed time.

The generated const service table dispatches SIDs without a central SID switch.
The DID table is sorted and searched with a bounded binary lookup. Multi-DID reads
preflight every descriptor, permission and cumulative length before any snapshot;
this profile rejects the entire request with NRC 0x31 for an unknown or
session-inaccessible DID. A callback failure produces a negative response, so
partial output is never published. Writes validate access, exact wire size,
finite numeric range and machine preconditions before one atomic business call.
The server retains a terminal response under backpressure without repeating the
business action. Positive-response suppression is supported for subfunctions;
negative responses remain visible subject to functional NRC suppression.

The FDCAN callback copies at most eight bytes into a bounded FIFO. Short,
board-owned critical sections protect FIFO operations; no CAN or business work
runs with interrupts masked. `APPUDS_Cyclic` subscribes directly through the public FMKFDCAN API after
FMKFDCAN completes its private node startup. The diagnostic module never changes node
clocks, bit rate, HAL instances or other subscribers. A full receive queue fails
closed until explicit reinitialization. Call module initialization once per boot
because the current framework has no unsubscribe API.

## Configuration generation

Excel is the only maintained DID/configuration list. `UDS_CodeGen.py` validates
named tables through the existing `LoadConfig_FromExcel` reader, then renders C/H
and JSON from one intermediate model. The main generation chain validates UDS
before existing generators run and calls UDS generation last. UDS owns the
PARAMETERS refresh, avoiding the old optional APPSPM JSON path, which cannot
represent the workbook's strings/structures correctly.

`Uds_GeneralCfg` adds `Target` and `CatalogueVersion` to the specified fields.
Target rows use these approved example profiles:

| Target | ECU | Physical RX / TX | RX / TX capacity | Logical staging region |
|---|---|---|---|---|
| STM32H753ZI | 0x11 | 0x7E0 / 0x7E8 | 512 / 512 | 1, disabled |
| STM32G474RE | 0x12 | 0x7E1 / 0x7E9 | 256 / 256 | 2, disabled |

`Uds_FlashCfg` also has `Target`. Neither row contains a physical address.
MaxImageSize is zero and SignatureRequired is true. BootloaderSupport is rejected
until the generator/board integration has explicitly approved production ports.

UDS numeric bounds can inherit APPSPM bounds or narrow them. Types, byte lengths,
scaling, access rights and NVM ownership must agree with the business definition.
Only compiled SID/routine symbols are accepted. CUSTOM adapters are declared in
the generated public specific header as `UDSAPP_Read_<Name>` and
`UDSAPP_Write_<Name>`; missing implementations fail at link time. Their callbacks
use the same documented key/buffer/size contract as built-in adapters.

All inputs are validated and all output content is rendered before replacement.
Generated sections use Start/End markers; bytes outside them are preserved,
including line endings. Files are staged before atomic per-file renames. A runtime
replacement failure restores prior outputs. Filesystem/power-loss atomicity
across several files is not claimed. Unchanged generation is byte-identical.
The pre-existing generators' unrelated output writes are not made transactional.

Regenerate the active H753 profile from the repository root:

```powershell
python Doc/ConfigPrj/PythonTool_CodeGen/App_CodeGen/UDS_CodeGen.py Doc/ConfigPrj/ExcelCfg/Project_SoftwareCfg_V0.xlsm Doc/ConfigPrj/ExcelCfg/STM32H753ZI/STM32H753ZI_HwCfg.xlsx Doc/ConfigPrj/UdsCfg/UdsInfo_V01Pr01.json
```

`tools/uds/check_targets.py` generates isolated profiles and catalogues for both
targets under `.pio/uds_targets/<target>/`, compiles the same core and its
generated tables for the respective ARM CPU, and checks bounded static RAM.
It also compiles the merged enabled H753 module against actual FMKFDCAN headers.
It does not replace the active H753 configuration with G474 definitions.

`tools/uds/add_excel_tables.py` is the one-time migration for an older workbook.
It refuses an existing UDS sheet. It preserves existing worksheet/table/macro ZIP
payloads instead of saving through openpyxl, which would remove validation
extensions. The reader's openpyxl validation warning does not imply a workbook
write. Formula cells must have valid cached results from Excel recalculation;
missing cached identifiers fail validation.

## Application policy and extensions

All UDS-specific configuration files are grouped under
`APP_CFG/ConfigSpecific/AppUdsConfigSpecific/`. `APP_UDS.c` owns the lifecycle,
RX FIFO, ISO-TP context and direct FMKFDCAN subscription/transmit callbacks.
The separate BoardPort source and header have been removed. Interrupt-mask
operations remain in generated `APPUDS_ConfigSpecific.c`; the module never
receives a HAL handle. The former `APPUDS_Fdcan.c/.h` files are also removed.
`APPSYS_ConfigPublic.h` includes `APPSYS_MODULE_APP_UDS`, and the private registry
directly binds `APPUDS_Init`, `APPUDS_Cyclic`, `APPUDS_GetState` and
`APPUDS_SetState`. `main.c` contains only APPSYS lifecycle handling.

The module links against FMKFDCAN even when the diagnostic profile is disabled.
`APPUDS_ENABLED` is a runtime profile selection: the disabled profile remains
BUSY without subscribing or sending frames. The same module source supports the
native FMKFDCAN implementation; PC simulation no longer suppresses its transport.
To remove the module from a build, remove its APPSYS registry entry and associated
configuration references as well as its source files.

Each registry entry carries `cyclicPeriodMs_u16`. APPSYS dispatches APPUDS every
1 ms in serialized main context while existing modules retain the system's
50 ms period. UDS business handlers never run from the timer ISR. Assertion reset
and watchdog servicing remain at the system period. Disabled UDS profiles
initialize with RC_OK and report BUSY, so they do not stop APPSYS initialization.
The independent protocol APIs are named `APPUDS_ServerInit`, `APPUDS_ServerCyclic`,
`APPUDS_ServerGetState` and `APPUDS_ServerSetState`, implemented in
`APPUDS_Server.c`; they support protocol tests independently of the FMKFDCAN module lifecycle.
NVM board configuration and backends are similarly grouped under
`FMK_CFG/FMKCFG_ConfigSpecific/FmkNvmConfigSpecific/`.

`AppUdsConfigSpecific/APPUDS_ApplicationPort.c` is manually owned and never regenerated.
Implement the machine-state check and an approved authentication backend there.
The defaults revoke all grants and deny maintenance. The authentication callback
may implement SID 0x27 or 0x29, but no trivial seed/key implementation is supplied.
An external backend must enforce attempt limits, delay, credential provisioning
and session-specific grants. Changing sessions or expiring S3 revokes grants.

For a business-approved writable DID, select `APPLICATION` as its Excel
Precondition and select authorized non-default sessions. `DENY` remains the
initial precondition. The global and per-DID checks both run before application.
Custom snapshot callbacks must use the business module's synchronization rules;
the server does not read private RAM or mask interrupts across a CAN operation.

To add a service, implement the common handler contract, add its known symbol to
the generator's compiled handler allowlist and add its Excel service row. Neither
the transport nor the core dispatch requires a new SID case. Add routines through
the equivalent bounded routine registry.

## Programming boundary

RequestDownload accepts exactly DFI 0x00 and ALFID 0x44: four big-endian bytes for a
logical region identifier, then four for image length. It negotiates the complete
TransferData PDU maximum, including SID and block counter. TransferData confirms
each backend commit before advancing, wraps counters modulo 256, and accepts a
repeat of only the immediately previous counter with identical length and bytes.
An altered duplicate receives NRC 0x73. TransferExit requires exact total length
and leaves the image at TRANSFER_COMPLETE, never bootable.

Routine 0xFF01 delegates the supplied manifest to a trusted verifier and reaches
VERIFIED only on successful verification. Routine 0xFF02 separately requests
activation. Mock activation and reset are always denied. The mock port stages at
most 4096 bytes in RAM. Its verifier must be installed explicitly; without one,
all verification fails. Tests inject HMAC/SHA-256 mock verification, confined to
the host test process. This is not a production signature scheme or credential.

The H753 and G474 Flash ports are separate, disabled contracts with distinct
logical regions and zero usable image capacity. They perform no hardware access.
Production work still needs independently validated linker partitions, protected
bootloaders, family-specific erase/program alignment, ECC/cache/bank handling,
trusted signature keys, anti-rollback, watchdog integration, power-safe metadata,
and an A/B or explicitly recoverable single-slot policy for each board. Pending
backend cancellation/recovery must be designed with those protected ports before
they are enabled.

## Python validation client

Install dependencies in a project-local environment, then run tests:

```powershell
python -m venv --system-site-packages .pio/uds_venv
.pio/uds_venv/Scripts/python.exe -m pip install -r tools/uds/requirements.txt
.pio/uds_venv/Scripts/python.exe -m unittest discover -s tests/uds -v
.pio/uds_venv/Scripts/python.exe tools/uds/check_targets.py
pio run
```

GCC must be on PATH for host C tests. The ARM compiler defaults to the installed
PlatformIO package and can be overridden with `--compiler`.

```powershell
python tools/uds/uds_tool.py info --ecu 0x11 --catalog Doc/ConfigPrj/UdsCfg/UdsInfo_V01Pr01.json
python tools/uds/uds_tool.py read --ecu 0x11 --did SYS_MACHINE_ID --catalog catalogue.json --interface socketcan --channel can0
python tools/uds/uds_tool.py write --ecu 0x11 --did SW_VERSION --value 120 --session EXTENDED --auth-plugin my_policy:authorize --catalog catalogue.json --interface pcan --channel PCAN_USBBUS1
python tools/uds/uds_tool.py flash --ecu 0x11 --package firmware.fwpkg --dry-run --catalog catalogue.json --log events.jsonl
```

`info`/`list` select the ECU explicitly and inspect its catalogue offline. Live
operations refuse disabled profiles. The authentication plugin is an explicitly
selected trusted local `module:function(client, required_level)` returning true
only after successful server authorization. No default plugin or unlock is
installed. The ECU remains authoritative for permissions and machine state.

A firmware package is a ZIP containing exactly `image.bin` and `manifest.json`.
The manifest contains `image_size`, `sha256`, `product_id`, `hw_revision` and
`signature`. Dry-run checks shape, identity and digest, reports that signature
authenticity was not checked, and never opens CAN or enters programming session.
Live transfers and routines require `--mock` plus an explicitly simulated test
catalogue with `firmware.backend=MOCK`; they never request activation. The
interoperability test exercises this full CLI transfer/verification path against
the C mock backend. JSONL logs contain request/response payloads and errors/NRCs.

## Validation and remaining acceptance work

The host suite contains 34 tests: actual compiled APPSYS registry dispatch,
main-context cyclic periods, absence of UDS calls in main, private CAN startup ownership and enabled/disabled/error
policies, deterministic/rollback generation, invalid
tables, business propagation and DID removal, legacy preservation, migration
preservation, protected/manual sections, main-chain wiring, scalar golden values,
permissions and atomic writes, P2/S3, asynchronous commit polling, malformed PDU
and CAN fuzz inputs, sequence wrap, FC errors, queue backpressure, simulated
download/duplicate/signature/reset failures, offline dry-run, and real library
interoperation over python-can's virtual bus.

| Acceptance group | Result / limit |
|---|---|
| GEN-01..05 | Covered by executed host tests |
| DID-01..05, UDS-01..02 | Covered with fake public business accessors and real C handlers |
| ISO-01 | Host vectors and can-isotp interoperability; on-bus timing remains HIL work |
| FW-01..02 | RAM backend, exact readback, asynchronous commits and rejected mock signatures |
| FW-03 | Simulated staging reset tested; physical power-cut campaign not executed |
| CAN-01 | Foreign identifier excluded by host port; actual shared-bus adapter filtering needs HIL |
| REG-01 | Existing H753 firmware and native PC simulation builds pass |
| PORT-01..02, PORT-04 | Shared core and generated tables compile for both CPUs with `-Werror`; H753 enabled adapter compile also passes |
| PORT-03, PORT-05 | No physical board diagnostic/Flash/recovery campaign executed |

FMKFDCAN currently exposes queue acceptance, not per-frame physical transmit
confirmation. ISO-TP deadlines therefore bound queue submission and inter-frame
progress using a common configured timeout; they do not independently prove
physical N_As/N_Ar completion. A queued-frame completion API and HIL timing checks
are required before claiming all ISO-TP timer guarantees or implementing safe
response-then-reset. ECUReset intentionally returns a standardized refusal.

The full project-wide hardware headers remain H753-generated. The G474 check is
compile-only for the shared core and tables, not a linked G474 application with
validated clocks, pin routing, interrupt/DMA configuration or shared CAN owner.
The 500 kbit/s profile, production authorization, exact firmware partitions and
both protected bootloaders remain explicit board-integration decisions.

Protocol/client implementation references: [Linux ISO-TP documentation](https://docs.kernel.org/networking/iso15765-2.html),
[can-isotp examples](https://can-isotp.readthedocs.io/en/latest/isotp/examples.html),
[udsoncan client](https://udsoncan.readthedocs.io/en/latest/udsoncan/client.html).
