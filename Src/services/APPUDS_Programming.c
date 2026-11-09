/** @file APPUDS_Programming.c @brief Nonblocking, logical-region-only programming lifecycle. */
#include "APPUDS_Programming.h"
#include <string.h>

// Local functions - Prototypes
/** @brief Recheck session, authorization, machine state and complete backend contract.
 * @param[in] f_Context_ps Server. @param[in,out] f_Exchange_ps NRC output.
 * @retval RC_OK Authorized. @retval RC_ERROR_NOT_ALLOWED Denied.
 */
static t_eReturnCode s_APPUDS_ProgrammingAllowed(const t_sAPPUDS_Context * f_Context_ps,
                                               t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Decode exactly four network-order bytes.
 * @param[in] f_Data_pu8 Borrowed bytes. @return Logical unsigned value.
 */
static t_uint32 s_APPUDS_ReadU32(const t_uint8 * f_Data_pu8);
/** @brief Format a successful routine response.
 * @param[in,out] f_Exchange_ps Borrowed request and output. @retval RC_OK Formatted.
 */
static t_eReturnCode s_APPUDS_RoutineResponse(t_sAPPUDS_Exchange * f_Exchange_ps);

/*********************************
 * APPUDS_ServiceDownload
 *********************************/
t_eReturnCode APPUDS_ServiceDownload(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = s_APPUDS_ProgrammingAllowed(f_Context_ps, f_Exchange_ps);

    //---- 1- Accept only the documented uncompressed 32-bit logical address and size profile ----//
    if(Ret_e == RC_OK)
    {
        if(f_Exchange_ps->requestSize_u16 != 11U)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
        }
        else if((f_Exchange_ps->request_pu8[1] != 0U) || (f_Exchange_ps->request_pu8[2] != 0x44U))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_RANGE;
        }
        else if((f_Exchange_ps->first_b == TRUE)
             && (f_Context_ps->programState_e != APPUDS_PROGRAM_IDLE)
             && (f_Context_ps->programState_e != APPUDS_PROGRAM_FAILED)
             && (f_Context_ps->programState_e != APPUDS_PROGRAM_AUTHORIZED))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SEQUENCE;
        }
        else
        {
            const t_sAPPUDS_FlashPort * port_ps = f_Context_ps->config_ps->flash_ps;
            t_uint32 region_u32 = s_APPUDS_ReadU32(&f_Exchange_ps->request_pu8[3]);
            t_uint32 size_u32 = s_APPUDS_ReadU32(&f_Exchange_ps->request_pu8[7]);

            if((region_u32 != port_ps->logicalRegion_u32) || (size_u32 == 0U)
             || (size_u32 > port_ps->maxImageSize_u32) || (port_ps->maxBlockLength_u16 < 3U)
             || (port_ps->maxBlockLength_u16 > APPUDS_RX_BUFFER_SIZE)
             || ((port_ps->maxBlockLength_u16 - 2U) > APPUDS_MAX_WRITE_PAYLOAD))
            {
                f_Exchange_ps->nrc_e = APPUDS_NRC_RANGE;
            }
            else
            {
                //---- 2- Prepare staging asynchronously; never acknowledge an unprepared region ----//
                f_Context_ps->programState_e = APPUDS_PROGRAM_DOWNLOAD_REQUESTED;
                Ret_e = port_ps->Begin_pcb(f_Exchange_ps->first_b, region_u32, NULL, size_u32);

                if(Ret_e == RC_OK)
                {
                    f_Context_ps->imageSize_u32 = size_u32;
                    f_Context_ps->received_u32 = 0U;
                    f_Context_ps->blockSequence_u8 = 1U;
                    f_Context_ps->previousSize_u16 = 0U;
                    f_Context_ps->programState_e = APPUDS_PROGRAM_RECEIVING;
                    f_Exchange_ps->response_pu8[1] = 0x20U;
                    f_Exchange_ps->response_pu8[2] = (t_uint8)(port_ps->maxBlockLength_u16 >> 8U);
                    f_Exchange_ps->response_pu8[3] = (t_uint8)port_ps->maxBlockLength_u16;
                    f_Exchange_ps->responseSize_u16 = 4U;
                }
                else if(Ret_e == RC_WARNING_PENDING)
                {
                    f_Context_ps->programState_e = APPUDS_PROGRAM_ERASING;
                }
                else
                {
                    f_Context_ps->programState_e = APPUDS_PROGRAM_FAILED;
                    f_Exchange_ps->nrc_e = APPUDS_NRC_DOWNLOAD;
                    Ret_e = RC_OK;
                }
            }
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceTransfer
 *********************************/
t_eReturnCode APPUDS_ServiceTransfer(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = s_APPUDS_ProgrammingAllowed(f_Context_ps, f_Exchange_ps);

    //---- 1- Validate the active transfer and exact block bounds before writing ----//
    if(Ret_e == RC_OK)
    {
        if((f_Exchange_ps->requestSize_u16 < 3U)
         || (f_Exchange_ps->requestSize_u16 > f_Context_ps->config_ps->flash_ps->maxBlockLength_u16))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
        }
        else if(f_Context_ps->programState_e != APPUDS_PROGRAM_RECEIVING)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SEQUENCE;
        }
        else
        {
            t_uint8 sequence_u8 = f_Exchange_ps->request_pu8[1];
            t_uint16 size_u16 = f_Exchange_ps->requestSize_u16 - 2U;
            t_uint8 previous_u8 = (t_uint8)(f_Context_ps->blockSequence_u8 - 1U);
            if((sequence_u8 == previous_u8) && (f_Context_ps->previousSize_u16 != 0U))
            {
                int comparison_s32 = 1;
                if(size_u16 == f_Context_ps->previousSize_u16)
                {
                    comparison_s32 = memcmp(f_Context_ps->previousBlock_au8,
                                             &f_Exchange_ps->request_pu8[2], size_u16);
                }
                if(comparison_s32 != 0)
                {
                    f_Exchange_ps->nrc_e = APPUDS_NRC_BLOCK_SEQUENCE;
                }
            }
            else if(sequence_u8 != f_Context_ps->blockSequence_u8)
            {
                f_Exchange_ps->nrc_e = APPUDS_NRC_BLOCK_SEQUENCE;
            }
            else if((size_u16 > APPUDS_MAX_WRITE_PAYLOAD)
                 || (size_u16 > f_Context_ps->imageSize_u32 - f_Context_ps->received_u32))
            {
                f_Exchange_ps->nrc_e = APPUDS_NRC_TRANSFER;
            }
            else
            {
                //---- 2- Advance counters only after the port confirms a durable commit ----//
                Ret_e = f_Context_ps->config_ps->flash_ps->Write_pcb(f_Exchange_ps->first_b,
                                                                   f_Context_ps->received_u32,
                                                                   &f_Exchange_ps->request_pu8[2], size_u16);
                if(Ret_e == RC_OK)
                {
                    memcpy(f_Context_ps->previousBlock_au8, &f_Exchange_ps->request_pu8[2], size_u16);
                    f_Context_ps->previousSize_u16 = size_u16;
                    f_Context_ps->received_u32 += size_u16;
                    f_Context_ps->blockSequence_u8++;
                }
                else if(Ret_e != RC_WARNING_PENDING)
                {
                    f_Context_ps->programState_e = APPUDS_PROGRAM_FAILED;
                    f_Exchange_ps->nrc_e = APPUDS_NRC_PROGRAMMING;
                    Ret_e = RC_OK;
                }
            }
            if((Ret_e == RC_OK) && (f_Exchange_ps->nrc_e == APPUDS_NRC_NONE))
            {
                f_Exchange_ps->response_pu8[1] = sequence_u8;
                f_Exchange_ps->responseSize_u16 = 2U;
            }
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_ServiceExit
 *********************************/
t_eReturnCode APPUDS_ServiceExit(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = s_APPUDS_ProgrammingAllowed(f_Context_ps, f_Exchange_ps);

    //---- 1- Require an exact complete image before closing staging ----//
    if(Ret_e == RC_OK)
    {
        if(f_Exchange_ps->requestSize_u16 != 1U)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
        }
        else if((f_Context_ps->programState_e != APPUDS_PROGRAM_RECEIVING)
             || (f_Context_ps->received_u32 != f_Context_ps->imageSize_u32))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SEQUENCE;
        }
        else
        {
            //---- 2- Finish never verifies or activates the image ----//
            Ret_e = f_Context_ps->config_ps->flash_ps->Finish_pcb(f_Exchange_ps->first_b, 0U, NULL, 0U);
            if(Ret_e == RC_OK)
            {
                f_Context_ps->programState_e = APPUDS_PROGRAM_TRANSFER_COMPLETE;
                f_Exchange_ps->responseSize_u16 = 1U;
            }
            else if(Ret_e != RC_WARNING_PENDING)
            {
                f_Context_ps->programState_e = APPUDS_PROGRAM_FAILED;
                f_Exchange_ps->nrc_e = APPUDS_NRC_PROGRAMMING;
                Ret_e = RC_OK;
            }
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_RoutineVerify
 *********************************/
t_eReturnCode APPUDS_RoutineVerify(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = s_APPUDS_ProgrammingAllowed(f_Context_ps, f_Exchange_ps);

    //---- 1- Accept explicit start verification only after transfer completion ----//
    if(Ret_e == RC_OK)
    {
        if((f_Exchange_ps->request_pu8[1] & 0x7FU) != 1U)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SUBFUNCTION;
        }
        else if(f_Exchange_ps->requestSize_u16 <= 4U)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
        }
        else if((f_Context_ps->programState_e != APPUDS_PROGRAM_TRANSFER_COMPLETE)
             && (f_Context_ps->programState_e != APPUDS_PROGRAM_VERIFYING))
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SEQUENCE;
        }
        else
        {
            //---- 2- Trust only the installed verifier, never TransferExit or a client assertion ----//
            f_Context_ps->programState_e = APPUDS_PROGRAM_VERIFYING;
            Ret_e = f_Context_ps->config_ps->flash_ps->Verify_pcb(f_Exchange_ps->first_b, 0U,
                                                                &f_Exchange_ps->request_pu8[4],
                                                                f_Exchange_ps->requestSize_u16 - 4U);
            if(Ret_e == RC_OK)
            {
                f_Context_ps->programState_e = APPUDS_PROGRAM_VERIFIED;
                Ret_e = s_APPUDS_RoutineResponse(f_Exchange_ps);
            }
            else if(Ret_e != RC_WARNING_PENDING)
            {
                f_Context_ps->programState_e = APPUDS_PROGRAM_FAILED;
                f_Exchange_ps->nrc_e = APPUDS_NRC_PROGRAMMING;
                Ret_e = RC_OK;
            }
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_RoutineActivate
 *********************************/
t_eReturnCode APPUDS_RoutineActivate(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = s_APPUDS_ProgrammingAllowed(f_Context_ps, f_Exchange_ps);

    //---- 1- Require verification and a real approved activation port ----//
    if(Ret_e == RC_OK)
    {
        if(f_Exchange_ps->requestSize_u16 != 4U)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_LENGTH;
        }
        else if((f_Exchange_ps->request_pu8[1] & 0x7FU) != 1U)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SUBFUNCTION;
        }
        else if(f_Context_ps->programState_e != APPUDS_PROGRAM_VERIFIED)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_SEQUENCE;
        }
        else if(f_Context_ps->config_ps->flash_ps->simulated_b == TRUE)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
        }
        else
        {
            //---- 2- Commit activation metadata through the protected bootloader contract ----//
            Ret_e = f_Context_ps->config_ps->flash_ps->Activate_pcb(f_Exchange_ps->first_b, 0U, NULL, 0U);
            if(Ret_e == RC_OK)
            {
                f_Context_ps->programState_e = APPUDS_PROGRAM_PENDING_ACTIVATION;
                Ret_e = s_APPUDS_RoutineResponse(f_Exchange_ps);
            }
            else if(Ret_e != RC_WARNING_PENDING)
            {
                f_Context_ps->programState_e = APPUDS_PROGRAM_FAILED;
                f_Exchange_ps->nrc_e = APPUDS_NRC_PROGRAMMING;
                Ret_e = RC_OK;
            }
        }
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_ProgrammingAllowed
 *********************************/
static t_eReturnCode s_APPUDS_ProgrammingAllowed(const t_sAPPUDS_Context * f_Context_ps,
                                               t_sAPPUDS_Exchange * f_Exchange_ps)
{
    t_eReturnCode Ret_e = RC_ERROR_NOT_ALLOWED;
    const t_sAPPUDS_FlashPort * port_ps = f_Context_ps->config_ps->flash_ps;

    //---- 1- Recheck every maintenance gate before each nonblocking backend step ----//
    if(f_Context_ps->session_u8 != 2U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_SERVICE_SESSION;
    }
    else if(f_Context_ps->security_u8 == 0U)
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_SECURITY;
    }
    else if((port_ps == NULL) || (f_Context_ps->config_ps->Check_pcb == NULL))
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
    }
    else if((port_ps->Begin_pcb == NULL) || (port_ps->Write_pcb == NULL) || (port_ps->Finish_pcb == NULL)
         || (port_ps->Verify_pcb == NULL) || (port_ps->Activate_pcb == NULL))
    {
        f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
    }
    else
    {
        Ret_e = f_Context_ps->config_ps->Check_pcb(0U);
        if(Ret_e != RC_OK)
        {
            f_Exchange_ps->nrc_e = APPUDS_NRC_CONDITIONS;
        }
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_ReadU32
 *********************************/
static t_uint32 s_APPUDS_ReadU32(const t_uint8 * f_Data_pu8)
{
    //---- 1- Assemble a logical value without unaligned pointer conversions ----//
    t_uint32 value_u32 = ((t_uint32)f_Data_pu8[0] << 24U) | ((t_uint32)f_Data_pu8[1] << 16U)
                       | ((t_uint32)f_Data_pu8[2] << 8U) | f_Data_pu8[3];

    return value_u32;
}

/*********************************
 * s_APPUDS_RoutineResponse
 *********************************/
static t_eReturnCode s_APPUDS_RoutineResponse(t_sAPPUDS_Exchange * f_Exchange_ps)
{
    //---- 1- Echo the accepted routine identifier and start subfunction ----//
    t_eReturnCode Ret_e = RC_OK;
    f_Exchange_ps->response_pu8[1] = 1U;
    f_Exchange_ps->response_pu8[2] = f_Exchange_ps->request_pu8[2];
    f_Exchange_ps->response_pu8[3] = f_Exchange_ps->request_pu8[3];
    f_Exchange_ps->responseSize_u16 = 4U;

    return Ret_e;
}
