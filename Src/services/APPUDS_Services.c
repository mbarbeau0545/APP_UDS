/** @file APPUDS_Services.c @brief Standard diagnostics using generated callback registries. */
#include "APPUDS_Services.h"

// Local functions - Prototypes
/** @brief Bounded binary lookup in the generated DID table.
 * @param[in] f_Context_ps Server. @param[in] f_Id_u16 DID.
 * @return Descriptor or null when absent.
 */
static const t_sAPPUDS_Did * s_APPUDS_FindDid(const t_sAPPUDS_Context * f_Context_ps, t_uint16 f_Id_u16);
/** @brief Evaluate DID session and security without invoking business operations.
 * @param[in] f_Context_ps Server. @param[in] f_Did_ps Descriptor or null. @param[in] f_Write_b Write access.
 * @return Standard NRC or APPUDS_NRC_NONE.
 */
static t_eAPPUDS_Nrc s_APPUDS_Access(const t_sAPPUDS_Context * f_Context_ps,
                                   const t_sAPPUDS_Did * f_Did_ps, t_bool f_Write_b);

/*********************************
 * APPUDS_ServiceSession
 *********************************/
t_eReturnCode APPUDS_ServiceSession(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Validate the complete subfunction before changing any state ----//
    if(f_Exchange_ps->requestSize_u16 != 2U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
    }
    else
    {
        t_uint8 session_u8 = f_Exchange_ps->request_pu8[1] & 0x7FU;
        if((session_u8 < 1U) || (session_u8 > 3U))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SUBFUNCTION;
        }
        else
        {
            if(session_u8 == 2U)
            {
                if(f_Context_ps->config_ps->Check_pcb == NULL)
                {
                    Ret_e = RC_ERROR_NOT_ALLOWED;
                }
                else
                {
                    Ret_e = f_Context_ps->config_ps->Check_pcb(0U);
                }
                if(Ret_e != RC_OK)
                {
                    f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
                }
            }
            if(f_Exchange_ps->nrc_e == APPUDS_NRC_NONE)
            {
                //---- 2- Revoke authorization and report P2 and P2* in UDS wire units ----//
                f_Context_ps->session_u8 = session_u8;
                f_Context_ps->security_u8 = 0U;
                f_Context_ps->programState_e = APPUDS_PROGRAM_IDLE;
                f_Context_ps->previousSize_u16 = 0U;
                t_uint16 p2_u16 = f_Context_ps->config_ps->p2Ms_u16;
                t_uint16 p2Star_u16 = (t_uint16)((f_Context_ps->config_ps->p2StarMs_u16 + 9U) / 10U);
                f_Exchange_ps->response_pu8[1] = session_u8;
                f_Exchange_ps->response_pu8[2] = (t_uint8)(p2_u16 >> 8U);
                f_Exchange_ps->response_pu8[3] = (t_uint8)p2_u16;
                f_Exchange_ps->response_pu8[4] = (t_uint8)(p2Star_u16 >> 8U);
                f_Exchange_ps->response_pu8[5] = (t_uint8)p2Star_u16;
                f_Exchange_ps->responseSize_u16 = 6U;
                f_Exchange_ps->suppress_b = (f_Exchange_ps->request_pu8[1] & 0x80U) != 0U;
            }
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceTester
 *********************************/
t_eReturnCode APPUDS_ServiceTester(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_OK;
    (void)f_Context_ps;

    //---- 1- Validate zero subfunction and honor suppression only for a positive response ----//
    if(f_Exchange_ps->requestSize_u16 != 2U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
    }
    else if((f_Exchange_ps->request_pu8[1] & 0x7FU) != 0U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_SUBFUNCTION;
    }
    else
    {
        f_Exchange_ps->response_pu8[1] = 0U;
        f_Exchange_ps->responseSize_u16 = 2U;
        f_Exchange_ps->suppress_b = (f_Exchange_ps->request_pu8[1] & 0x80U) != 0U;
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceRead
 *********************************/
t_eReturnCode APPUDS_ServiceRead(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_OK;
    t_uint16 responseSize_u16 = 1U;

    //---- 1- Preflight every DID and cumulative length before any business read ----//
    if((f_Exchange_ps->requestSize_u16 < 3U) || ((f_Exchange_ps->requestSize_u16 & 1U) == 0U))
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
    }
    else
    {
        for(t_uint16 pos_u16 = 1U; (pos_u16 < f_Exchange_ps->requestSize_u16)
                                && (f_Exchange_ps->nrc_e == APPUDS_NRC_NONE); pos_u16 += 2U)
        {
            t_uint16 id_u16 = (t_uint16)((f_Exchange_ps->request_pu8[pos_u16] << 8U)
                                      | f_Exchange_ps->request_pu8[pos_u16 + 1U]);
            const t_sAPPUDS_Did * did_ps = s_APPUDS_FindDid(f_Context_ps, id_u16);
            f_Exchange_ps->nrc_e = s_APPUDS_Access(f_Context_ps, did_ps, FALSE);
            if(f_Exchange_ps->nrc_e == APPUDS_NRC_NONE)
            {
                if((t_uint32)responseSize_u16 + 2U + did_ps->size_u16 > f_Exchange_ps->capacity_u16)
                {
                    f_Exchange_ps->nrc_e = APPUDS_NRC_RESPONSE_TOO_LONG;
                }
                else
                {
                    responseSize_u16 += 2U + did_ps->size_u16;
                }
            }
        }
    }

    //---- 2- Serialize coherent callbacks in request order after successful preflight ----//
    t_uint16 offset_u16 = 1U;
    for(t_uint16 pos_u16 = 1U; (pos_u16 < f_Exchange_ps->requestSize_u16)
                            && (f_Exchange_ps->nrc_e == APPUDS_NRC_NONE); pos_u16 += 2U)
    {
        t_uint16 id_u16 = (t_uint16)((f_Exchange_ps->request_pu8[pos_u16] << 8U)
                                  | f_Exchange_ps->request_pu8[pos_u16 + 1U]);
        const t_sAPPUDS_Did * did_ps = s_APPUDS_FindDid(f_Context_ps, id_u16);
        f_Exchange_ps->response_pu8[offset_u16] = (t_uint8)(id_u16 >> 8U);
        f_Exchange_ps->response_pu8[offset_u16 + 1U] = (t_uint8)id_u16;
        Ret_e = did_ps->Read_pcb(did_ps->key_u16, &f_Exchange_ps->response_pu8[offset_u16 + 2U],
                                 did_ps->size_u16);
        if(Ret_e != RC_OK)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
        }
        offset_u16 += 2U + did_ps->size_u16;
    }
    f_Exchange_ps->responseSize_u16 = responseSize_u16;
    Ret_e = RC_OK;

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceWrite
 *********************************/
t_eReturnCode APPUDS_ServiceWrite(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Resolve exactly one DID before length, authorization and value validation ----//
    if(f_Exchange_ps->requestSize_u16 < 4U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
    }
    else
    {
        t_uint16 id_u16 = (t_uint16)((f_Exchange_ps->request_pu8[1] << 8U) | f_Exchange_ps->request_pu8[2]);
        const t_sAPPUDS_Did * did_ps = s_APPUDS_FindDid(f_Context_ps, id_u16);
        f_Exchange_ps->nrc_e = s_APPUDS_Access(f_Context_ps, did_ps, TRUE);
        if(f_Exchange_ps->nrc_e == APPUDS_NRC_NONE)
        {
            if((f_Exchange_ps->requestSize_u16 != did_ps->size_u16 + 3U)
             || (did_ps->size_u16 > APPUDS_MAX_WRITE_PAYLOAD))
            {
                f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
            }
            else
            {
                double value_f64 = 0.0;
                Ret_e = APPUDS_DecodeValue(did_ps, &f_Exchange_ps->request_pu8[3], &value_f64);
                if(Ret_e != RC_OK)
                {
                    f_Exchange_ps->nrc_e = APPUDS_NRC_RANGE;
                }
                else if((did_ps->Check_pcb == NULL) || (f_Context_ps->config_ps->Check_pcb == NULL))
                {
                    f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
                }
                else
                {
                    Ret_e = f_Context_ps->config_ps->Check_pcb(did_ps->key_u16);
                    if(Ret_e == RC_OK)
                    {
                        Ret_e = did_ps->Check_pcb(did_ps->key_u16);
                    }
                    if(Ret_e != RC_OK)
                    {
                        f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
                    }
                    else
                    {
                        //---- 2- Apply once and acknowledge only effective business success ----//
                        Ret_e = did_ps->Write_pcb(did_ps->key_u16, &f_Exchange_ps->request_pu8[3], did_ps->size_u16);
                        if(Ret_e != RC_OK)
                        {
                            f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
                        }
                        else
                        {
                            f_Exchange_ps->response_pu8[1] = (t_uint8)(id_u16 >> 8U);
                            f_Exchange_ps->response_pu8[2] = (t_uint8)id_u16;
                            f_Exchange_ps->responseSize_u16 = 3U;
                        }
                    }
                }
            }
        }
    }
    Ret_e = RC_OK;

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceSecurity
 *********************************/
t_eReturnCode APPUDS_ServiceSecurity(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Delegate authentication without embedding a seed/key bypass ----//
    if(f_Exchange_ps->requestSize_u16 < 2U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
    }
    else if(f_Context_ps->config_ps->Authenticate_pcb == NULL)
    {
        f_Context_ps->security_u8 = 0U;
        f_Exchange_ps->nrc_e = APPUDS_NRC_SECURITY;
    }
    else
    {
        Ret_e = f_Context_ps->config_ps->Authenticate_pcb(f_Context_ps, f_Exchange_ps);
        if((Ret_e < RC_OK) || (f_Exchange_ps->nrc_e != APPUDS_NRC_NONE))
        {
            f_Context_ps->security_u8 = 0U;
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceReset
 *********************************/
t_eReturnCode APPUDS_ServiceReset(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_OK;
    (void)f_Context_ps;

    //---- 1- Keep reset fail-closed until physical TX confirmation is available ----//
    f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
    if(f_Exchange_ps->requestSize_u16 != 2U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
    }
    else if((f_Exchange_ps->request_pu8[1] & 0x7FU) != 1U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_SUBFUNCTION;
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceRoutine
 *********************************/
t_eReturnCode APPUDS_ServiceRoutine(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Resolve a registered routine without dispatching an arbitrary identifier ----//
    if(f_Exchange_ps->requestSize_u16 < 4U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
    }
    else
    {
        t_uint8 subfunction_u8 = f_Exchange_ps->request_pu8[1] & 0x7FU;
        t_uint16 id_u16 = (t_uint16)((f_Exchange_ps->request_pu8[2] << 8U) | f_Exchange_ps->request_pu8[3]);
        const t_sAPPUDS_Routine * routine_ps = NULL;
        for(t_uint16 idx_u16 = 0U; idx_u16 < f_Context_ps->config_ps->routineCount_u16; idx_u16++)
        {
            if(f_Context_ps->config_ps->routines_ps[idx_u16].id_u16 == id_u16)
            {
                routine_ps = &f_Context_ps->config_ps->routines_ps[idx_u16];
            }
        }
        if((subfunction_u8 < 1U) || (subfunction_u8 > 3U))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SUBFUNCTION;
        }
        else if(routine_ps == NULL)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_RANGE;
        }
        else if((routine_ps->sessions_u8 & (1U << (f_Context_ps->session_u8 - 1U))) == 0U)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_RANGE;
        }
        else if(f_Context_ps->security_u8 < routine_ps->security_u8)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SECURITY;
        }
        else if((routine_ps->Check_pcb == NULL) || (f_Context_ps->config_ps->Check_pcb == NULL))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
        }
        else
        {
            //---- 2- Recheck machine state on each asynchronous routine step ----//
            Ret_e = f_Context_ps->config_ps->Check_pcb(id_u16);
            if(Ret_e == RC_OK)
            {
                Ret_e = routine_ps->Check_pcb(id_u16);
            }
            if(Ret_e != RC_OK)
            {
                f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
                Ret_e = RC_OK;
            }
            else
            {
                f_Exchange_ps->suppress_b = (f_Exchange_ps->request_pu8[1] & 0x80U) != 0U;
                Ret_e = routine_ps->Handler_pcb(f_Context_ps, f_Exchange_ps);
            }
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_DenyMaintenance
 *********************************/
t_eReturnCode APPUDS_DenyMaintenance(t_uint16 f_Key_u16)
{
    //---- 1- Require an explicit application authorization policy before maintenance ----//
    t_eReturnCode Ret_e = RC_ERROR_NOT_ALLOWED;
    (void)f_Key_u16;

    return Ret_e;
}

/*********************************
 * s_APPUDS_FindDid
 *********************************/
static const t_sAPPUDS_Did * s_APPUDS_FindDid(const t_sAPPUDS_Context * f_Context_ps, t_uint16 f_Id_u16)
{
    const t_sAPPUDS_Did * found_ps = NULL;
    t_uint16 low_u16 = 0U;
    t_uint16 high_u16 = f_Context_ps->config_ps->didCount_u16;

    //---- 1- Search the sorted table with a bounded binary lookup ----//
    while((low_u16 < high_u16) && (found_ps == NULL))
    {
        t_uint16 middle_u16 = low_u16 + (high_u16 - low_u16) / 2U;
        const t_sAPPUDS_Did * did_ps = &f_Context_ps->config_ps->dids_ps[middle_u16];
        if(did_ps->did_u16 == f_Id_u16)
        {
            found_ps = did_ps;
        }
        else if(did_ps->did_u16 < f_Id_u16)
        {
            low_u16 = middle_u16 + 1U;
        }
        else
        {
            high_u16 = middle_u16;
        }
    }

    return found_ps;
}

/*********************************
 * s_APPUDS_Access
 *********************************/
static t_eAPPUDS_Nrc s_APPUDS_Access(const t_sAPPUDS_Context * f_Context_ps,
                                   const t_sAPPUDS_Did * f_Did_ps, t_bool f_Write_b)
{
    t_eAPPUDS_Nrc nrc_e = APPUDS_NRC_NONE;

    //---- 1- Refuse absent accessors before exposing any business value ----//
    if(f_Did_ps == NULL)
    {
        nrc_e = APPUDS_NRC_RANGE;
    }
    else if(((f_Write_b == TRUE) && (f_Did_ps->Write_pcb == NULL))
         || ((f_Write_b == FALSE) && (f_Did_ps->Read_pcb == NULL)))
    {
        nrc_e = APPUDS_NRC_RANGE;
    }
    else
    {
        t_uint8 sessions_u8 = f_Did_ps->readSessions_u8;
        t_uint8 security_u8 = f_Did_ps->readSecurity_u8;
        if(f_Write_b == TRUE)
        {
            sessions_u8 = f_Did_ps->writeSessions_u8;
            security_u8 = f_Did_ps->writeSecurity_u8;
        }
        if((sessions_u8 & (1U << (f_Context_ps->session_u8 - 1U))) == 0U)
        {
            nrc_e = APPUDS_NRC_RANGE;
        }
        else if(f_Context_ps->security_u8 < security_u8)
        {
            nrc_e = APPUDS_NRC_SECURITY;
        }
    }

    return nrc_e;
}
