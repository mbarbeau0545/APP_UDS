/** @file APPUDS_Server.c @brief Static UDS dispatch, lifecycle and response deadlines. */
#include "APP_UDS.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

/// @brief Persistent singleton server state.
static t_sAPPUDS_Context g_Context_s;
/// @brief Public lifecycle state.
static t_eCyclicModState g_State_e = STATE_CYCLIC_CFG;
/// @brief Server-owned copied request.
static t_uint8 g_Request_au8[APPUDS_RX_BUFFER_SIZE];
/// @brief Server-owned response retained through transport backpressure.
static t_uint8 g_Response_au8[APPUDS_TX_BUFFER_SIZE];
/// @brief Current exchange metadata.
static t_sAPPUDS_Exchange g_Exchange_s;
/// @brief Current compiled service.
static const t_sAPPUDS_Service * g_Service_ps;
/// @brief Application request reservation.
static t_bool g_Active_b;
/// @brief Terminal response waiting for transport acceptance.
static t_bool g_Ready_b;
/// @brief First cyclic invocation for a newly reserved request.
static t_bool g_Started_b;
/// @brief Last accepted request time for S3.
static t_uint32 g_LastRequest_u32;
/// @brief Request start tick.
static t_uint32 g_Start_u32;
/// @brief Last response or pending tick.
static t_uint32 g_ResponseTick_u32;
/// @brief Pending interval currently in use.
static t_uint32 g_Interval_u32;

// Local functions - Prototypes
/** @brief Validate generated tables before retaining configuration.
 * @param[in] f_Config_ps Persistent configuration.
 * @retval RC_OK Valid. @retval RC_ERROR_PARAM_INVALID Invalid descriptor.
 */
static t_eReturnCode s_APPUDS_Validate(const t_sAPPUDS_Config * f_Config_ps);
/** @brief Enter the configured lifecycle. @retval RC_OK Initialized. */
static t_eReturnCode s_APPUDS_Config(void);
/** @brief Run a bounded request step. @retval RC_OK Completed. @retval RC_WARNING_PENDING Active. */
static t_eReturnCode s_APPUDS_Operational(void);
/** @brief Hold a nonoperational lifecycle without hardware access. @retval RC_WARNING_NO_OPERATION Held. */
static t_eReturnCode s_APPUDS_Hold(void);
/** @brief Resolve and authorize a request in the generated service registry. @retval RC_OK Resolved. */
static t_eReturnCode s_APPUDS_Dispatch(void);
/** @brief Publish terminal or pending response with retry on backpressure.
 * @retval RC_OK Accepted. @retval RC_WARNING_BUSY Retained.
 */
static t_eReturnCode s_APPUDS_Respond(void);

/*********************************
 * APPUDS_ServerInit
 *********************************/
t_eReturnCode APPUDS_ServerInit(const t_sAPPUDS_Config * f_Config_ps)
{
    //---- 1- Validate all retained immutable descriptors before arming ----//
    t_eReturnCode Ret_e = s_APPUDS_Validate(f_Config_ps);
    if(Ret_e == RC_OK)
    {
        memset(&g_Context_s, 0, sizeof(g_Context_s));
        memset(&g_Exchange_s, 0, sizeof(g_Exchange_s));
        g_Context_s.config_ps = f_Config_ps;
        g_Context_s.session_u8 = 1U;
        g_State_e = STATE_CYCLIC_CFG;
        g_Active_b = FALSE;
        g_Ready_b = FALSE;
        g_Started_b = FALSE;
        g_Service_ps = NULL;
    }

    return Ret_e;
}

/*********************************
 * APPUDS_OnPayload
 *********************************/
t_eReturnCode APPUDS_OnPayload(const t_uint8 * f_Data_pu8, t_uint16 f_Size_u16, t_bool f_Functional_b)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Preserve the one active request and its response reservation ----//
    if((f_Data_pu8 == NULL) || (f_Size_u16 == 0U) || (f_Size_u16 > APPUDS_RX_BUFFER_SIZE)
     || (g_Context_s.config_ps == NULL) || (g_State_e != STATE_CYCLIC_OPE))
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else if(g_Active_b == TRUE)
    {
        Ret_e = RC_WARNING_BUSY;
    }
    else
    {
        //---- 2- Copy input and defer every business operation to Cyclic ----//
        memcpy(g_Request_au8, f_Data_pu8, f_Size_u16);
        memset(&g_Exchange_s, 0, sizeof(g_Exchange_s));
        g_Exchange_s.request_pu8 = g_Request_au8;
        g_Exchange_s.requestSize_u16 = f_Size_u16;
        g_Exchange_s.response_pu8 = g_Response_au8;
        g_Exchange_s.capacity_u16 = APPUDS_TX_BUFFER_SIZE;
        g_Exchange_s.first_b = TRUE;
        g_Exchange_s.functional_b = f_Functional_b;
        g_Active_b = TRUE;
        g_Started_b = FALSE;
        g_Ready_b = FALSE;
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServerCyclic
 *********************************/
t_eReturnCode APPUDS_ServerCyclic(t_uint32 f_Now_u32)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Select one public lifecycle step and centralize lifecycle transitions ----//
    g_Context_s.now_u32 = f_Now_u32;
    if(g_Context_s.config_ps == NULL)
    {
        Ret_e = RC_ERROR_INSTANCE_NOT_INITIALIZED;
    }
    else
    {
        switch(g_State_e)
        {
            case STATE_CYCLIC_CFG:
            {
                Ret_e = s_APPUDS_Config();
                if(Ret_e == RC_OK)
                {
                    g_State_e = STATE_CYCLIC_OPE;
                }
                else if(Ret_e < RC_OK)
                {
                    g_State_e = STATE_CYCLIC_ERROR;
                }
                break;
            }
            case STATE_CYCLIC_OPE:
            {
                Ret_e = s_APPUDS_Operational();
                if(Ret_e < RC_OK)
                {
                    g_State_e = STATE_CYCLIC_ERROR;
                    g_Context_s.security_u8 = 0U;
                }
                break;
            }
            default:
            {
                Ret_e = s_APPUDS_Hold();
                break;
            }
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServerGetState
 *********************************/
t_eReturnCode APPUDS_ServerGetState(t_eCyclicModState * f_State_pe)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Publish the lifecycle without exposing mutable server state ----//
    if(f_State_pe == NULL)
    {
        Ret_e = RC_ERROR_PTR_NULL;
    }
    else
    {
        *f_State_pe = g_State_e;
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServerSetState
 *********************************/
t_eReturnCode APPUDS_ServerSetState(t_eCyclicModState f_State_e)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Revoke authorization whenever operational processing is suspended ----//
    if((t_uint32)f_State_e > (t_uint32)STATE_CYCLIC_ERROR)
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else
    {
        g_State_e = f_State_e;
        if(f_State_e != STATE_CYCLIC_OPE)
        {
            g_Active_b = FALSE;
            g_Context_s.security_u8 = 0U;
            g_Context_s.session_u8 = 1U;
            g_Context_s.programState_e = APPUDS_PROGRAM_FAILED;
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_DecodeValue
 *********************************/
t_eReturnCode APPUDS_DecodeValue(const t_sAPPUDS_Did * f_Did_ps, const t_uint8 * f_Data_pu8,
                                double * f_Value_pf64)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Reject invalid descriptors before accessing wire bytes ----//
    if((f_Did_ps == NULL) || (f_Data_pu8 == NULL) || (f_Value_pf64 == NULL))
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else if(f_Did_ps->type_e == APPUDS_TYPE_BYTES)
    {
        *f_Value_pf64 = 0.0;
    }
    else if((f_Did_ps->size_u16 == 0U) || (f_Did_ps->size_u16 > 4U)
         || ((t_uint32)f_Did_ps->type_e >= (t_uint32)APPUDS_TYPE_NB))
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else
    {
        //---- 2- Assemble the scalar without alignment or host-endian assumptions ----//
        uint32_t raw_u32 = 0U;
        for(t_uint16 idx_u16 = 0U; idx_u16 < f_Did_ps->size_u16; idx_u16++)
        {
            t_uint16 pos_u16 = idx_u16;
            if(f_Did_ps->bigEndian_b == FALSE)
            {
                pos_u16 = f_Did_ps->size_u16 - 1U - idx_u16;
            }
            raw_u32 = (raw_u32 << 8U) | f_Data_pu8[pos_u16];
        }
        double value_f64 = raw_u32;
        if(f_Did_ps->type_e == APPUDS_TYPE_FLOAT32)
        {
            float value_f32 = 0.0F;
            memcpy(&value_f32, &raw_u32, sizeof(value_f32));
            value_f64 = value_f32;
        }
        else if((f_Did_ps->type_e >= APPUDS_TYPE_SINT8) && (f_Did_ps->type_e <= APPUDS_TYPE_SINT32))
        {
            t_uint8 bits_u8 = (t_uint8)(f_Did_ps->size_u16 * 8U);
            uint32_t sign_u32 = (uint32_t)1U << (bits_u8 - 1U);
            if((raw_u32 & sign_u32) != 0U)
            {
                value_f64 -= (double)((uint64_t)1U << bits_u8);
            }
        }

        //---- 3- Reject nonfinite values and bounds violations before business writes ----//
        int finite_s32 = isfinite(value_f64);
        if((finite_s32 == 0) || (value_f64 < f_Did_ps->min_f64) || (value_f64 > f_Did_ps->max_f64))
        {
            Ret_e = RC_ERROR_PARAM_INVALID;
        }
        else
        {
            *f_Value_pf64 = value_f64;
        }
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_Validate
 *********************************/
static t_eReturnCode s_APPUDS_Validate(const t_sAPPUDS_Config * f_Config_ps)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Validate persistent table pointers and timing domains ----//
    if(f_Config_ps == NULL)
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else if((f_Config_ps->Send_pcb == NULL) || (f_Config_ps->p2Ms_u16 == 0U)
         || (f_Config_ps->p2StarMs_u16 < f_Config_ps->p2Ms_u16)
         || (f_Config_ps->s3Ms_u32 <= f_Config_ps->p2StarMs_u16)
         || (f_Config_ps->s3Ms_u32 > 0x7FFFFFFFUL)
         || ((f_Config_ps->didCount_u16 != 0U) && (f_Config_ps->dids_ps == NULL))
         || ((f_Config_ps->serviceCount_u16 != 0U) && (f_Config_ps->services_ps == NULL))
         || ((f_Config_ps->routineCount_u16 != 0U) && (f_Config_ps->routines_ps == NULL)))
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else
    {
        //---- 2- Require sorted unique DIDs with representable response lengths ----//
        for(t_uint16 idx_u16 = 0U; (idx_u16 < f_Config_ps->didCount_u16) && (Ret_e == RC_OK); idx_u16++)
        {
            const t_sAPPUDS_Did * did_ps = &f_Config_ps->dids_ps[idx_u16];
            int minFinite_s32 = isfinite(did_ps->min_f64);
            int maxFinite_s32 = isfinite(did_ps->max_f64);
            t_uint16 scalarSize_u16 = 0U;
            if((did_ps->type_e == APPUDS_TYPE_UINT8) || (did_ps->type_e == APPUDS_TYPE_SINT8))
            {
                scalarSize_u16 = 1U;
            }
            else if((did_ps->type_e == APPUDS_TYPE_UINT16) || (did_ps->type_e == APPUDS_TYPE_SINT16))
            {
                scalarSize_u16 = 2U;
            }
            else if(did_ps->type_e != APPUDS_TYPE_BYTES)
            {
                scalarSize_u16 = 4U;
            }
            if((did_ps->size_u16 == 0U) || (did_ps->size_u16 > (APPUDS_TX_BUFFER_SIZE - 3U))
             || ((t_uint32)did_ps->type_e >= (t_uint32)APPUDS_TYPE_NB)
             || (did_ps->min_f64 > did_ps->max_f64) || (minFinite_s32 == 0) || (maxFinite_s32 == 0)
             || ((scalarSize_u16 != 0U) && (did_ps->size_u16 != scalarSize_u16)))
            {
                Ret_e = RC_ERROR_PARAM_INVALID;
            }
            else if((idx_u16 > 0U) && (did_ps->did_u16 <= f_Config_ps->dids_ps[idx_u16 - 1U].did_u16))
            {
                Ret_e = RC_ERROR_PARAM_INVALID;
            }
        }
        for(t_uint16 idx_u16 = 0U; (idx_u16 < f_Config_ps->routineCount_u16) && (Ret_e == RC_OK); idx_u16++)
        {
            const t_sAPPUDS_Routine * routine_ps = &f_Config_ps->routines_ps[idx_u16];
            if((routine_ps->Handler_pcb == NULL) || (routine_ps->sessions_u8 == 0U))
            {
                Ret_e = RC_ERROR_PARAM_INVALID;
            }
            for(t_uint16 prior_u16 = 0U; prior_u16 < idx_u16; prior_u16++)
            {
                if(f_Config_ps->routines_ps[prior_u16].id_u16 == routine_ps->id_u16)
                {
                    Ret_e = RC_ERROR_PARAM_INVALID;
                }
            }
        }
        for(t_uint16 idx_u16 = 0U; (idx_u16 < f_Config_ps->serviceCount_u16) && (Ret_e == RC_OK); idx_u16++)
        {
            const t_sAPPUDS_Service * service_ps = &f_Config_ps->services_ps[idx_u16];
            if((service_ps->Handler_pcb == NULL) || (service_ps->sessions_u8 == 0U)
             || (service_ps->timeoutMs_u32 < f_Config_ps->p2Ms_u16)
             || (service_ps->timeoutMs_u32 > 0x7FFFFFFFUL))
            {
                Ret_e = RC_ERROR_PARAM_INVALID;
            }
            for(t_uint16 prior_u16 = 0U; prior_u16 < idx_u16; prior_u16++)
            {
                if(f_Config_ps->services_ps[prior_u16].sid_u8 == service_ps->sid_u8)
                {
                    Ret_e = RC_ERROR_PARAM_INVALID;
                }
            }
        }
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_Config
 *********************************/
static t_eReturnCode s_APPUDS_Config(void)
{
    //---- 1- Initialize session timing in cyclic context ----//
    t_eReturnCode Ret_e = RC_OK;
    g_LastRequest_u32 = g_Context_s.now_u32;

    return Ret_e;
}

/*********************************
 * s_APPUDS_Hold
 *********************************/
static t_eReturnCode s_APPUDS_Hold(void)
{
    //---- 1- Preserve lifecycle until explicitly resumed ----//
    t_eReturnCode Ret_e = RC_WARNING_NO_OPERATION;

    return Ret_e;
}

/*********************************
 * s_APPUDS_Dispatch
 *********************************/
static t_eReturnCode s_APPUDS_Dispatch(void)
{
    t_eReturnCode Ret_e = RC_OK;
    t_uint8 sid_u8 = g_Request_au8[0];

    //---- 1- Resolve an immutable service entry without a central SID switch ----//
    g_Service_ps = NULL;
    for(t_uint16 idx_u16 = 0U; idx_u16 < g_Context_s.config_ps->serviceCount_u16; idx_u16++)
    {
        if(g_Context_s.config_ps->services_ps[idx_u16].sid_u8 == sid_u8)
        {
            g_Service_ps = &g_Context_s.config_ps->services_ps[idx_u16];
        }
    }
    t_bool dangerous_b = (sid_u8 == 0x2EU) || (sid_u8 == 0x34U) || (sid_u8 == 0x36U)
                      || (sid_u8 == 0x37U) || (sid_u8 == 0x11U);
    t_uint8 sessionMask_u8 = (t_uint8)(1U << (g_Context_s.session_u8 - 1U));

    //---- 2- Enforce addressing, session and authorization before invoking a handler ----//
    if(g_Service_ps == NULL)
    {
        g_Exchange_s.nrc_e = APPUDS_NRC_SERVICE;
    }
    else if((g_Exchange_s.functional_b == TRUE)
         && ((dangerous_b == TRUE) || (g_Service_ps->functional_b == FALSE)))
    {
        g_Exchange_s.nrc_e = APPUDS_NRC_SERVICE;
    }
    else if((g_Service_ps->sessions_u8 & sessionMask_u8) == 0U)
    {
        g_Exchange_s.nrc_e = APPUDS_NRC_SERVICE_SESSION;
    }
    else if(g_Context_s.security_u8 < g_Service_ps->security_u8)
    {
        g_Exchange_s.nrc_e = APPUDS_NRC_SECURITY;
    }
    g_Ready_b = (g_Exchange_s.nrc_e != APPUDS_NRC_NONE);

    return Ret_e;
}

/*********************************
 * s_APPUDS_Operational
 *********************************/
static t_eReturnCode s_APPUDS_Operational(void)
{
    t_eReturnCode Ret_e = RC_OK;
    uint32_t idle_u32 = (uint32_t)(g_Context_s.now_u32 - g_LastRequest_u32);

    //---- 1- Expire idle sessions and revoke stale programming authorization ----//
    if(((g_Active_b == FALSE) || (g_Started_b == FALSE))
     && (idle_u32 >= g_Context_s.config_ps->s3Ms_u32))
    {
        g_Context_s.session_u8 = 1U;
        g_Context_s.security_u8 = 0U;
        g_Context_s.programState_e = APPUDS_PROGRAM_IDLE;
        g_Context_s.previousSize_u16 = 0U;
    }
    if(g_Active_b == TRUE)
    {
        if(g_Started_b == FALSE)
        {
            g_Start_u32 = g_Context_s.now_u32;
            g_ResponseTick_u32 = g_Context_s.now_u32;
            g_LastRequest_u32 = g_Context_s.now_u32;
            g_Interval_u32 = g_Context_s.config_ps->p2Ms_u16;
            g_Started_b = TRUE;
            Ret_e = s_APPUDS_Dispatch();
        }

        //---- 2- Execute one nonblocking handler step within its absolute deadline ----//
        if(g_Ready_b == FALSE)
        {
            uint32_t elapsed_u32 = (uint32_t)(g_Context_s.now_u32 - g_Start_u32);
            if(elapsed_u32 >= g_Service_ps->timeoutMs_u32)
            {
                g_Exchange_s.nrc_e = APPUDS_NRC_GENERAL_REJECT;
                g_Context_s.programState_e = APPUDS_PROGRAM_FAILED;
                g_Context_s.security_u8 = 0U;
                g_Ready_b = TRUE;
            }
            else
            {
                Ret_e = g_Service_ps->Handler_pcb(&g_Context_s, &g_Exchange_s);
                g_Exchange_s.first_b = FALSE;
                if(Ret_e != RC_WARNING_PENDING)
                {
                    if((Ret_e != RC_OK) && (g_Exchange_s.nrc_e == APPUDS_NRC_NONE))
                    {
                        g_Exchange_s.nrc_e = APPUDS_NRC_CONDITIONS;
                    }
                    if((g_Exchange_s.nrc_e == APPUDS_NRC_NONE)
                     && ((g_Exchange_s.responseSize_u16 == 0U)
                      || (g_Exchange_s.responseSize_u16 > APPUDS_TX_BUFFER_SIZE)))
                    {
                        g_Exchange_s.nrc_e = APPUDS_NRC_RESPONSE_TOO_LONG;
                    }
                    g_Ready_b = TRUE;
                    g_ResponseTick_u32 = g_Context_s.now_u32;
                }
            }
        }

        //---- 3- Retain terminal and pending responses until transport acceptance ----//
        Ret_e = s_APPUDS_Respond();
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_Respond
 *********************************/
static t_eReturnCode s_APPUDS_Respond(void)
{
    t_eReturnCode Ret_e = RC_WARNING_PENDING;
    t_uint8 negative_au8[3] = {0x7FU, g_Request_au8[0], (t_uint8)g_Exchange_s.nrc_e};
    uint32_t elapsed_u32 = (uint32_t)(g_Context_s.now_u32 - g_ResponseTick_u32);

    //---- 1- Apply functional NRC suppression and positive-response suppression ----//
    t_bool silent_b = (g_Exchange_s.nrc_e == APPUDS_NRC_NONE) && (g_Exchange_s.suppress_b == TRUE);
    if((g_Exchange_s.functional_b == TRUE)
     && ((g_Exchange_s.nrc_e == APPUDS_NRC_SERVICE) || (g_Exchange_s.nrc_e == APPUDS_NRC_SUBFUNCTION)
      || (g_Exchange_s.nrc_e == APPUDS_NRC_RANGE) || (g_Exchange_s.nrc_e == APPUDS_NRC_SUBFUNCTION_SESSION)
      || (g_Exchange_s.nrc_e == APPUDS_NRC_SERVICE_SESSION)))
    {
        silent_b = TRUE;
    }
    if(g_Ready_b == TRUE)
    {
        if(silent_b == TRUE)
        {
            Ret_e = RC_OK;
        }
        else if(g_Exchange_s.nrc_e != APPUDS_NRC_NONE)
        {
            Ret_e = g_Context_s.config_ps->Send_pcb(negative_au8, 3U);
        }
        else
        {
            g_Response_au8[0] = g_Request_au8[0] + 0x40U;
            Ret_e = g_Context_s.config_ps->Send_pcb(g_Response_au8, g_Exchange_s.responseSize_u16);
        }
        if(Ret_e == RC_OK)
        {
            g_Active_b = FALSE;
            g_LastRequest_u32 = g_Context_s.now_u32;
        }
        else if(elapsed_u32 >= g_Context_s.config_ps->p2StarMs_u16)
        {
            Ret_e = RC_ERROR_TIMEOUT;
        }
    }
    else if(elapsed_u32 >= g_Interval_u32)
    {
        //---- 2- ResponsePending never discards the original operation ----//
        negative_au8[2] = APPUDS_NRC_PENDING;
        Ret_e = g_Context_s.config_ps->Send_pcb(negative_au8, 3U);
        if(Ret_e == RC_OK)
        {
            g_ResponseTick_u32 = g_Context_s.now_u32;
            g_Interval_u32 = g_Context_s.config_ps->p2StarMs_u16;
            Ret_e = RC_WARNING_PENDING;
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServerHasPending
 *********************************/
t_eReturnCode APPUDS_ServerHasPending(t_bool * f_Pending_pb)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Query the owned request reservation without invoking a business callback ----//
    if(f_Pending_pb == NULL)
    {
        Ret_e = RC_ERROR_PTR_NULL;
    }
    else
    {
        *f_Pending_pb = g_Active_b;
    }

    return Ret_e;
}
