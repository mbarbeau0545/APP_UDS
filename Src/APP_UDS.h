/** @file APP_UDS.h
 *  @brief FMKFDCAN-dependent diagnostic module and allocation-free UDS server interfaces.
 */
#ifndef APPUDS_H_INCLUDED
#define APPUDS_H_INCLUDED

#include "TypeCommon.h"
#include "APP_CFG/ConfigFiles/APPUDS_ConfigPublic.h"

/// @brief UDS negative response codes; zero represents a positive result.
typedef enum
{
    APPUDS_NRC_NONE = 0x00,
    APPUDS_NRC_GENERAL_REJECT = 0x10,
    APPUDS_NRC_SERVICE = 0x11,
    APPUDS_NRC_SUBFUNCTION = 0x12,
    APPUDS_NRC_LENGTH = 0x13,
    APPUDS_NRC_RESPONSE_TOO_LONG = 0x14,
    APPUDS_NRC_BUSY = 0x21,
    APPUDS_NRC_CONDITIONS = 0x22,
    APPUDS_NRC_SEQUENCE = 0x24,
    APPUDS_NRC_RANGE = 0x31,
    APPUDS_NRC_SECURITY = 0x33,
    APPUDS_NRC_INVALID_KEY = 0x35,
    APPUDS_NRC_ATTEMPTS = 0x36,
    APPUDS_NRC_DELAY = 0x37,
    APPUDS_NRC_DOWNLOAD = 0x70,
    APPUDS_NRC_TRANSFER = 0x71,
    APPUDS_NRC_PROGRAMMING = 0x72,
    APPUDS_NRC_BLOCK_SEQUENCE = 0x73,
    APPUDS_NRC_PENDING = 0x78,
    APPUDS_NRC_SUBFUNCTION_SESSION = 0x7E,
    APPUDS_NRC_SERVICE_SESSION = 0x7F,
    APPUDS_NRC_NB = 0x80
} t_eAPPUDS_Nrc;

/// @brief Fixed wire types supported by the generator.
typedef enum
{
    APPUDS_TYPE_UINT8 = 0,
    APPUDS_TYPE_UINT16,
    APPUDS_TYPE_UINT32,
    APPUDS_TYPE_SINT8,
    APPUDS_TYPE_SINT16,
    APPUDS_TYPE_SINT32,
    APPUDS_TYPE_FLOAT32,
    APPUDS_TYPE_BYTES,
    APPUDS_TYPE_NB
} t_eAPPUDS_DataType;

/// @brief Programming lifecycle; incomplete or merely transferred images are never activatable.
typedef enum
{
    APPUDS_PROGRAM_IDLE = 0,
    APPUDS_PROGRAM_AUTHORIZED,
    APPUDS_PROGRAM_DOWNLOAD_REQUESTED,
    APPUDS_PROGRAM_ERASING,
    APPUDS_PROGRAM_RECEIVING,
    APPUDS_PROGRAM_TRANSFER_COMPLETE,
    APPUDS_PROGRAM_VERIFYING,
    APPUDS_PROGRAM_VERIFIED,
    APPUDS_PROGRAM_PENDING_ACTIVATION,
    APPUDS_PROGRAM_FAILED,
    APPUDS_PROGRAM_NB
} t_eAPPUDS_ProgramState;

/**
 * @brief Read a DID into wire bytes in cyclic context.
 * @note The callback must obtain a coherent business snapshot; it may not retain the buffer.
 * @param[in] f_Key_u16 : Generated business identifier.
 * @param[out] f_Data_pu8 : Output buffer owned by the server.
 * @param[in] f_Size_u16 : Exact required wire size; never exceed it.
 * @retval RC_OK Exactly the requested bytes were encoded.
 * @retval RC_WARNING_BUSY Value unavailable; no successful response is sent.
 */
typedef t_eReturnCode t_cbAPPUDS_Read(t_uint16 f_Key_u16, t_uint8 * f_Data_pu8, t_uint16 f_Size_u16);
/**
 * @brief Apply an already validated DID value atomically in cyclic context.
 * @note No input pointer may be retained. RC_OK confirms application of the value.
 *       For APPSPM_NVM_DEFERRED it confirms RAM only; APPSPM owns later NVM publication.
 * @param[in] f_Key_u16 : Generated business identifier.
 * @param[in] f_Data_pu8 : Borrowed exact-size wire value.
 * @param[in] f_Size_u16 : Exact wire length.
 * @retval RC_OK Applied.
 * @retval RC_WARNING_BUSY Not applied; caller may retry a new request.
 * @retval RC_ERROR_NOT_ALLOWED Business precondition failed; unchanged.
 */
typedef t_eReturnCode t_cbAPPUDS_Write(t_uint16 f_Key_u16, const t_uint8 * f_Data_pu8, t_uint16 f_Size_u16);
/**
 * @brief Check an application maintenance precondition in cyclic context.
 * @note No hardware work may block this callback. The application owns its state.
 * @param[in] f_Key_u16 : Business or routine identifier.
 * @retval RC_OK Safe to proceed.
 * @retval RC_ERROR_NOT_ALLOWED Operation must be denied.
 */
typedef t_eReturnCode t_cbAPPUDS_Precondition(t_uint16 f_Key_u16);

/// @brief One generated DID, sorted by its identifier.
typedef struct
{
    t_uint16 did_u16;                    ///< Unique wire identifier.
    t_uint16 key_u16;                    ///< Business identifier, never a memory address.
    t_uint16 size_u16;                   ///< Exact wire size.
    t_eAPPUDS_DataType type_e;           ///< Wire scalar type or explicit byte array.
    t_bool bigEndian_b;                  ///< True for network byte order.
    t_uint8 readSessions_u8;             ///< Bit 0 default, bit 1 programming, bit 2 extended.
    t_uint8 writeSessions_u8;            ///< Allowed write sessions.
    t_uint8 readSecurity_u8;             ///< Minimum read authorization.
    t_uint8 writeSecurity_u8;            ///< Minimum write authorization.
    double min_f64;                      ///< Canonical numeric lower bound.
    double max_f64;                      ///< Canonical numeric upper bound.
    t_cbAPPUDS_Read * Read_pcb;          ///< Null means not readable.
    t_cbAPPUDS_Write * Write_pcb;        ///< Null means not writable.
    t_cbAPPUDS_Precondition * Check_pcb; ///< Required for writes; null denies.
} t_sAPPUDS_Did;

/// @brief Shared server state passed to bounded service handlers.
typedef struct __t_sAPPUDS_Context t_sAPPUDS_Context;

/// @brief Borrowed service exchange, valid only within a cyclic callback.
typedef struct
{
    const t_uint8 * request_pu8; ///< Internal immutable request copy including SID.
    t_uint16 requestSize_u16;    ///< Exact request length.
    t_uint8 * response_pu8;      ///< Internal output including positive SID.
    t_uint16 capacity_u16;       ///< Output capacity.
    t_uint16 responseSize_u16;   ///< Handler-produced positive response length.
    t_eAPPUDS_Nrc nrc_e;         ///< Handler-produced standardized negative response code.
    t_bool first_b;              ///< True on the first invocation of this request.
    t_bool functional_b;         ///< Functional addressing; dangerous services are always denied.
    t_bool suppress_b;           ///< Suppress positive response only.
} t_sAPPUDS_Exchange;

/**
 * @brief Execute one bounded step of a UDS service in cyclic context.
 * @note Pending callbacks are re-entered with first_b false. No borrowed exchange pointer may be retained.
 *       A terminal result sets nrc_e or responseSize_u16. Do not repeat side effects after first_b.
 * @param[in,out] f_Context_ps : Persistent server state.
 * @param[in,out] f_Exchange_ps : Borrowed request and bounded response.
 * @retval RC_OK Terminal result.
 * @retval RC_WARNING_PENDING More work required; server handles P2/P2* responses.
 * @retval RC_ERROR_NOT_ALLOWED Backend denied the operation.
 */
typedef t_eReturnCode t_cbAPPUDS_Service(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);

/// @brief Generated extensible service registration.
typedef struct
{
    t_uint8 sid_u8;                   ///< Wire service identifier.
    t_uint8 sessions_u8;              ///< Allowed session mask.
    t_uint8 security_u8;              ///< Required authorization level.
    t_bool functional_b;              ///< Explicit functional permission.
    t_uint32 timeoutMs_u32;           ///< Absolute service execution bound.
    t_cbAPPUDS_Service * Handler_pcb; ///< Compiled handler; never an arbitrary Excel expression.
} t_sAPPUDS_Service;

/// @brief Generated routine registration.
typedef struct
{
    t_uint16 id_u16;                     ///< Routine identifier.
    t_uint8 sessions_u8;                 ///< Allowed session mask.
    t_uint8 security_u8;                 ///< Required authorization.
    t_cbAPPUDS_Precondition * Check_pcb; ///< Mandatory safe-machine check.
    t_cbAPPUDS_Service * Handler_pcb;    ///< Bounded start/stop/results handler.
} t_sAPPUDS_Routine;

/**
 * @brief Submit an outgoing UDS payload to a copying transport in cyclic context.
 * @note The callback copies before returning. RC_WARNING_BUSY must have no side effect.
 * @param[in] f_Data_pu8 : Borrowed response bytes.
 * @param[in] f_Size_u16 : Exact response length.
 * @retval RC_OK Accepted.
 * @retval RC_WARNING_BUSY Retry later.
 */
typedef t_eReturnCode t_cbAPPUDS_Send(const t_uint8 * f_Data_pu8, t_uint16 f_Size_u16);

/**
 * @brief Perform one asynchronous logical programming operation.
 * @note Used by Begin/Write/Finish/Verify/Activate. The port owns persistent state.
 *       On first_b it must copy input if retained; subsequent invocations poll the same operation.
 *       Verify must check a trusted signature, digest, compatibility and anti-rollback before RC_OK.
 * @param[in] f_First_b : Begin operation or poll completion.
 * @param[in] f_Offset_u32 : Logical region for Begin; image offset for Write.
 * @param[in] f_Data_pu8 : Borrowed block or verification manifest; null for operations with no data.
 * @param[in] f_Size_u32 : Image length for Begin; input byte count otherwise.
 * @retval RC_OK Operation durably completed; for mock ports RAM completion only.
 * @retval RC_WARNING_PENDING Poll again.
 * @retval RC_ERROR_NOT_ALLOWED Unsafe or unverified operation.
 * @retval RC_ERROR_WRONG_RESULT Verification or programming failed.
 */
typedef t_eReturnCode t_cbAPPUDS_Flash(t_bool f_First_b, t_uint32 f_Offset_u32,
                                     const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32);

/// @brief Abstract bootloader port, never containing caller-controlled physical addresses.
typedef struct
{
    t_uint32 logicalRegion_u32;      ///< Board-approved logical region identifier.
    t_uint32 maxImageSize_u32;       ///< Bound checked before any operation.
    t_uint16 maxBlockLength_u16;     ///< Maximum TransferData PDU size including SID and counter.
    t_bool simulated_b;              ///< Mock backend indicator, advertised in tooling.
    t_cbAPPUDS_Flash * Begin_pcb;    ///< Prepare approved staging region.
    t_cbAPPUDS_Flash * Write_pcb;    ///< Durable sequential write.
    t_cbAPPUDS_Flash * Finish_pcb;   ///< Close incomplete, non-bootable staging image.
    t_cbAPPUDS_Flash * Verify_pcb;   ///< Trusted signature and image policy verification.
    t_cbAPPUDS_Flash * Activate_pcb; ///< Atomically request a verified image activation.
} t_sAPPUDS_FlashPort;

/// @brief Immutable server configuration; all referenced tables outlive the server.
typedef struct
{
    const t_sAPPUDS_Did * dids_ps;         ///< Sorted DID table.
    t_uint16 didCount_u16;                 ///< DID count.
    const t_sAPPUDS_Service * services_ps; ///< Unique service table.
    t_uint16 serviceCount_u16;             ///< Service count.
    const t_sAPPUDS_Routine * routines_ps; ///< Routine table.
    t_uint16 routineCount_u16;             ///< Routine count.
    t_uint16 p2Ms_u16;                     ///< Initial response deadline.
    t_uint16 p2StarMs_u16;                 ///< Pending response interval.
    t_uint32 s3Ms_u32;                     ///< Session inactivity limit.
    t_cbAPPUDS_Send * Send_pcb;            ///< Copying response transport.
    t_cbAPPUDS_Precondition * Check_pcb;   ///< Global maintenance precondition, null denies.
    t_cbAPPUDS_Service * Authenticate_pcb; ///< Trusted authentication handler, null denies.
    const t_sAPPUDS_FlashPort * flash_ps;  ///< Null disables programming.
} t_sAPPUDS_Config;

/// @brief Server state shared with compiled handlers; application access goes through public APIs.
struct __t_sAPPUDS_Context
{
    const t_sAPPUDS_Config * config_ps;                  ///< Persistent immutable configuration.
    t_uint8 session_u8;                                  ///< Current wire session, 1..3.
    t_uint8 security_u8;                                 ///< Trusted authentication backend grant.
    t_uint32 now_u32;                                    ///< Current monotone tick.
    t_eAPPUDS_ProgramState programState_e;               ///< Logical programming lifecycle.
    t_uint32 imageSize_u32;                              ///< Expected complete image size.
    t_uint32 received_u32;                               ///< Durably committed bytes.
    t_uint8 blockSequence_u8;                            ///< Next block counter, modulo 256.
    t_uint8 previousBlock_au8[APPUDS_MAX_WRITE_PAYLOAD]; ///< Exact prior block for duplicate comparison.
    t_uint16 previousSize_u16;                           ///< Prior committed data length.
};

/**
 * @brief Arm the configured UDS module through the APPSYS lifecycle registry.
 * @note No hardware operation is performed. Disabled profiles initialize successfully in BUSY state.
 * @retval RC_OK Module armed or intentionally disabled.
 * @retval RC_ERROR_PARAM_INVALID Invalid generated configuration.
 */
t_eReturnCode APPUDS_Init(void);
/**
 * @brief Progress FMKFDCAN transport and diagnostic processing in APPSYS main context.
 * @note Normal lifecycle calls use the system period. Active exchanges also use a 1 ms main-context
 *       FastTask registered with APPSYS. This function must never run from an interrupt callback.
 * @retval RC_OK Progressed.
 * @retval RC_WARNING_PENDING Waiting for startup or asynchronous work.
 * @retval RC_WARNING_NO_OPERATION Disabled or suspended.
 * @retval RC_ERROR_WRONG_STATE Failed module.
 */
t_eReturnCode APPUDS_Cyclic(void);
/** @brief Query the configured module lifecycle for APPSYS monitoring.
 * @param[out] f_State_pe Module state, including transport startup and errors.
 * @retval RC_OK Returned. @retval RC_ERROR_PTR_NULL Null output.
 */
t_eReturnCode APPUDS_GetState(t_eCyclicModState * f_State_pe);
/** @brief Control the configured module lifecycle from serialized main context.
 * @param[in] f_State_e Requested lifecycle state.
 * @retval RC_OK Applied. @retval RC_WARNING_PENDING Transport startup is not complete.
 * @retval RC_ERROR_PARAM_INVALID Invalid state.
 */
t_eReturnCode APPUDS_SetState(t_eCyclicModState f_State_e);

/**
 * @brief Submit one server response to the module-owned ISO-TP transport.
 * @note Internal configuration callback, used from serialized cyclic context. Copies bytes before returning;
 *       no caller pointer is retained. The enabled module must be operational.
 * @param[in] f_Data_pu8 Borrowed response bytes.
 * @param[in] f_Size_u16 Exact response length.
 * @retval RC_OK Response copied.
 * @retval RC_WARNING_BUSY Previous response still active; retry later.
 * @retval RC_ERROR_NOT_ALLOWED Disabled or nonoperational module.
 */
t_eReturnCode APPUDS_SendPayload_PRIVATE_(const t_uint8 * f_Data_pu8, t_uint16 f_Size_u16);

/**
 * @brief Arm the independent singleton server; initialization performs no hardware operation.
 * @note Standalone ports and host tests use this API. Do not independently drive a server owned by APPSYS.
 * @param[in] f_Config_ps : Persistent immutable validated configuration.
 * @retval RC_OK Armed in CFG state.
 * @retval RC_ERROR_PARAM_INVALID Invalid table or timing configuration.
 */
t_eReturnCode APPUDS_ServerInit(const t_sAPPUDS_Config * f_Config_ps);
/**
 * @brief Copy one received PDU; callbacks and business operations run later in Cyclic.
 * @param[in] f_Data_pu8 : Input bytes; never retained.
 * @param[in] f_Size_u16 : Exact PDU length.
 * @param[in] f_Functional_b : Addressing metadata supplied by a trusted transport.
 * @retval RC_OK Copied.
 * @retval RC_WARNING_BUSY One request is already active; input is not overwritten.
 * @retval RC_ERROR_PARAM_INVALID Invalid PDU or server state.
 */
t_eReturnCode APPUDS_OnPayload(const t_uint8 * f_Data_pu8, t_uint16 f_Size_u16, t_bool f_Functional_b);
/**
 * @brief Progress one server step with wrap-safe time and bounded P2 and extended P2 deadlines.
 * @param[in] f_Now_u32 : Monotone millisecond tick.
 * @retval RC_OK Progressed.
 * @retval RC_WARNING_PENDING Request active.
 * @retval RC_WARNING_BUSY Transport backpressure.
 */
t_eReturnCode APPUDS_ServerCyclic(t_uint32 f_Now_u32);
/** @brief Query lifecycle state. @param[out] f_State_pe State output.
 * @retval RC_OK Returned. @retval RC_ERROR_PTR_NULL Null output.
 */
t_eReturnCode APPUDS_ServerGetState(t_eCyclicModState * f_State_pe);
/**
 * @brief Query whether a copied request or retained response needs another serialized server step.
 * @note Called from the same main context as ServerCyclic; no business operation executes here.
 * @param[out] f_Pending_pb TRUE while a server request remains reserved.
 * @retval RC_OK Activity returned.
 * @retval RC_ERROR_PTR_NULL Null output.
 */
t_eReturnCode APPUDS_ServerHasPending(t_bool * f_Pending_pb);
/** @brief Set lifecycle state and discard authorization and incomplete operations when leaving OPE.
 * @param[in] f_State_e New state. @retval RC_OK Applied. @retval RC_ERROR_PARAM_INVALID Invalid state.
 */
t_eReturnCode APPUDS_ServerSetState(t_eCyclicModState f_State_e);

/** @brief Validate a wire scalar without modifying the business value.
 * @param[in] f_Did_ps Descriptor. @param[in] f_Data_pu8 Exact-size bytes.
 * @param[out] f_Value_pf64 Decoded scalar, zero for BYTES.
 * @retval RC_OK Valid. @retval RC_ERROR_PARAM_INVALID Invalid type, range or nonfinite value.
 */
t_eReturnCode APPUDS_DecodeValue(const t_sAPPUDS_Did * f_Did_ps, const t_uint8 * f_Data_pu8,
                                double * f_Value_pf64);

#endif
